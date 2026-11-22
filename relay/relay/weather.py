"""IP-located five-day forecast for the reader's calendar page.

The device never talks to a weather provider and has no city setting: it asks
the relay, and the relay decides where "here" is from the caller's public
address. That keeps the relay the single backend (SCOPE.md) and means a device
that moves between networks gets the right weather without any configuration.

Location resolution, in order (see resolve_client_ip + WeatherStore.get):

1. The caller's public address, as seen by the relay. Behind the shipped nginx
   site this is already ``request.client.host`` — uvicorn runs with
   ``--proxy-headers`` trusted for 127.0.0.1 and rewrites it from
   X-Forwarded-For. The explicit XFF fallback here covers a deployment without
   that proxy chain.
2. When the caller's address is private/loopback (relay and device on the same
   LAN) the relay's own egress address is used: on that topology "where the
   device is" and "where the relay is" are the same place.
3. WEATHER_LAT/WEATHER_LON, when set, skip all of the above.

Geocoding tries ip.sb, then ipinfo, then ip-api. The order is a reachability
fact, not a preference: the relay runs in mainland China, where ip-api.com does
not answer at all and api.ipify.org returns nothing, while ip.sb and ipinfo.io
both do. Providers disagree about smaller Chinese cities often enough that one
is not enough, and the relay has nothing else to correct them with — the
returned name is used verbatim, and the card shows a city, never a district.
"""

from __future__ import annotations

import ipaddress
import logging
import time
from collections import OrderedDict
from dataclasses import dataclass

import httpx

from .config import Settings

log = logging.getLogger("relay.weather")

IPSB_GEO_URL = "https://api.ip.sb/geoip/{ip}"
IPSB_IP_URL = "https://api.ip.sb/ip"
IPINFO_URL = "https://ipinfo.io/{ip}/json"
IPINFO_SELF_URL = "https://ipinfo.io/json"
IPAPI_URL = "http://ip-api.com/json/{ip}"
IPAPI_FIELDS = "status,message,city,regionName,lat,lon"
OPEN_METEO_URL = "https://api.open-meteo.com/v1/forecast"

# One geocode per source address per day: a device reconnecting all evening
# must not spend the provider's rate limit, and addresses rarely move.
GEO_TTL_S = 86400.0
MAX_GEO_KEYS = 64
MAX_CACHE_KEYS = 64

# open-meteo weather codes collapsed to the eight categories the firmware can
# name. Anything unrecognised is reported as "cloudy" rather than dropped: a
# forecast line with a temperature and no condition reads better than a gap.
WEATHER_CATEGORIES = 8
_WMO_TO_CATEGORY = {
    0: 0, 1: 0,                                   # clear
    2: 1,                                         # partly cloudy
    3: 2,                                         # overcast
    45: 3, 48: 3,                                 # fog
    51: 4, 53: 4, 55: 4, 56: 4, 57: 4,            # drizzle
    61: 4, 63: 5, 65: 5, 66: 5, 67: 5,            # rain
    80: 4, 81: 5, 82: 5,                          # showers
    71: 6, 73: 6, 75: 6, 77: 6, 85: 6, 86: 6,     # snow
    95: 7, 96: 7, 99: 7,                          # thunder
}
UNKNOWN_CATEGORY = 2


class WeatherError(Exception):
    def __init__(self, code: str, status: int = 502):
        super().__init__(code)
        self.code, self.status = code, status


@dataclass(frozen=True)
class Place:
    city: str
    lat: float
    lon: float


def resolve_client_ip(forwarded_for: str | None, client_host: str | None) -> str:
    """The caller's address: first X-Forwarded-For hop, else the socket peer.

    XFF is a comma-separated chain where the leftmost entry is the originating
    client. Empty/whitespace entries are skipped — some proxies emit ", " when
    they have nothing to add.
    """
    for candidate in (forwarded_for or "").split(","):
        value = candidate.strip()
        if value:
            return value
    return (client_host or "").strip()


def is_private_address(host: str) -> bool:
    """True for loopback/RFC1918/link-local — anything that cannot be located.

    An unparsable host also counts as private: a garbage value must fall back
    to the relay's own address, not be sent to a geocoder.
    """
    try:
        address = ipaddress.ip_address(host)
    except ValueError:
        return True
    return address.is_private or address.is_loopback or address.is_link_local or address.is_unspecified


def category_for(wmo_code: object) -> int:
    try:
        return _WMO_TO_CATEGORY.get(int(wmo_code), UNKNOWN_CATEGORY)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return UNKNOWN_CATEGORY


def _as_int(value: object, default: int = 0) -> int:
    try:
        return int(round(float(value)))  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return default


class WeatherStore:
    """Geocode with a day TTL, forecast with the configured TTL, per city.

    Only one refresh per city runs at a time: five devices in one household
    should produce one upstream call per TTL window, not five.
    """

    def __init__(self, settings: Settings):
        self.settings = settings
        self.geo: OrderedDict[str, tuple[float, Place]] = OrderedDict()
        self.cache: OrderedDict[str, tuple[float, dict]] = OrderedDict()
        self._inflight: dict[str, object] = {}
        self._egress_ip: str | None = None

    # ---------------------------------------------------------------- helpers
    def _client(self, timeout: float) -> httpx.AsyncClient:
        # trust_env=False: an ambient proxy setting must not break weather (the
        # same rule MarketStore follows).
        return httpx.AsyncClient(timeout=timeout, follow_redirects=False, trust_env=False)

    async def _get_text(self, url: str, params: dict | None = None, timeout: float | None = None) -> str:
        try:
            async with self._client(timeout or self.settings.weather_timeout_s) as client:
                response = await client.get(url, params=params)
        except httpx.HTTPError as exc:
            raise WeatherError("upstream_unavailable") from exc
        if response.status_code != 200:
            raise WeatherError("upstream_status", 502)
        return response.text

    async def _get_json(self, url: str, params: dict | None = None, timeout: float | None = None) -> dict:
        import orjson

        text = await self._get_text(url, params, timeout)
        try:
            payload = orjson.loads(text.encode())
        except orjson.JSONDecodeError as exc:
            raise WeatherError("upstream_payload") from exc
        if not isinstance(payload, dict):
            raise WeatherError("upstream_payload")
        return payload

    def _pinned_place(self) -> Place | None:
        pin = self.settings.weather_pin
        if pin is None:
            return None
        return Place(self.settings.weather_city.strip(), pin[0], pin[1])

    async def _egress_address(self) -> str:
        """The relay's own public address (LAN deployments, pinned IP unknown).

        ip.sb answers with the bare address; ipinfo's self lookup is the backup
        for the same reason the geocoders are ordered — ipify is not reachable
        from the production host.
        """
        if self._egress_ip:
            return self._egress_ip
        for fetch in (self._egress_ipsb, self._egress_ipinfo):
            try:
                address = await fetch()
            except WeatherError:
                continue
            if address:
                self._egress_ip = address
                return address
        return ""

    async def _egress_ipsb(self) -> str:
        return (await self._get_text(IPSB_IP_URL, timeout=self.settings.weather_geo_timeout_s)).strip()

    async def _egress_ipinfo(self) -> str:
        payload = await self._get_json(IPINFO_SELF_URL, timeout=self.settings.weather_geo_timeout_s)
        return str(payload.get("ip", "")).strip()

    # --------------------------------------------------------------- geocode
    async def _geocode_ipsb(self, address: str) -> Place | None:
        payload = await self._get_json(IPSB_GEO_URL.format(ip=address),
                                       timeout=self.settings.weather_geo_timeout_s)
        return self._place_from(payload.get("city"), payload.get("region"),
                                payload.get("latitude"), payload.get("longitude"))

    async def _geocode_ipinfo(self, address: str) -> Place | None:
        payload = await self._get_json(IPINFO_URL.format(ip=address),
                                       timeout=self.settings.weather_geo_timeout_s)
        loc = str(payload.get("loc", ""))
        lat, _, lon = loc.partition(",")
        return self._place_from(payload.get("city"), payload.get("region"), lat, lon)

    async def _geocode_ipapi(self, address: str) -> Place | None:
        payload = await self._get_json(
            IPAPI_URL.format(ip=address), params={"fields": IPAPI_FIELDS},
            timeout=self.settings.weather_geo_timeout_s,
        )
        if str(payload.get("status", "")) == "fail":
            return None
        return self._place_from(payload.get("city"), payload.get("regionName"),
                                payload.get("lat"), payload.get("lon"))

    @staticmethod
    def _place_from(city: object, region: object, lat: object, lon: object) -> Place | None:
        name = str(city or "").strip() or str(region or "").strip()
        try:
            lat_f, lon_f = float(lat), float(lon)  # type: ignore[arg-type]
        except (TypeError, ValueError):
            return None
        if not name or not (-90.0 <= lat_f <= 90.0) or not (-180.0 <= lon_f <= 180.0):
            return None
        return Place(name, lat_f, lon_f)

    async def place_for(self, address: str) -> Place:
        pinned = self._pinned_place()
        if pinned is not None:
            return pinned

        if not address or is_private_address(address):
            # LAN deployment: the caller's address says nothing about where it
            # is, and geocoding a private range returns the provider's guess at
            # the relay's own datacenter. Use the relay's egress address — on
            # this topology the two machines are in the same place anyway.
            try:
                address = await self._egress_address()
            except WeatherError as exc:
                raise WeatherError("geo_failed") from exc
            if not address:
                raise WeatherError("geo_failed")

        now = time.time()
        hit = self.geo.get(address)
        if hit is not None and now - hit[0] < GEO_TTL_S:
            self.geo.move_to_end(address)
            return hit[1]

        errors: list[str] = []
        for geocoder in (self._geocode_ipsb, self._geocode_ipinfo, self._geocode_ipapi):
            try:
                place = await geocoder(address)
            except WeatherError as exc:
                errors.append(exc.code)
                continue
            if place is None:
                errors.append("no_match")
                continue
            self.geo[address] = (now, place)
            self.geo.move_to_end(address)
            while len(self.geo) > MAX_GEO_KEYS:
                self.geo.popitem(last=False)
            log.info("weather geo ok", extra={"city": place.city, "source": geocoder.__name__})
            return place

        log.warning("weather geo failed", extra={"errors": errors})
        # A stale geocode beats no forecast at all — a moved address still
        # resolves to the last place this device was seen (six-hour forecast
        # windows make a day-old city a small risk).
        if hit is not None:
            return hit[1]
        raise WeatherError("geo_failed")

    # ---------------------------------------------------------------- forecast
    async def _fetch_forecast(self, place: Place, days: int) -> dict:
        payload = await self._get_json(
            OPEN_METEO_URL,
            params={
                "latitude": f"{place.lat:.4f}",
                "longitude": f"{place.lon:.4f}",
                "current": "temperature_2m,weather_code",
                "daily": "temperature_2m_max,temperature_2m_min,weather_code,precipitation_probability_max",
                "timezone": "auto",
                "forecast_days": str(days),
            },
        )
        daily = payload.get("daily")
        if not isinstance(daily, dict):
            raise WeatherError("upstream_payload")
        dates = daily.get("time")
        highs = daily.get("temperature_2m_max")
        lows = daily.get("temperature_2m_min")
        codes = daily.get("weather_code")
        rain = daily.get("precipitation_probability_max")
        if not (isinstance(dates, list) and isinstance(highs, list) and isinstance(lows, list)):
            raise WeatherError("upstream_payload")

        out_days = []
        for index, day in enumerate(dates[:days]):
            if index >= len(highs) or index >= len(lows):
                break
            out_days.append(
                {
                    "d": str(day),
                    "hi": _as_int(highs[index]),
                    "lo": _as_int(lows[index]),
                    "w": category_for(codes[index] if isinstance(codes, list) and index < len(codes) else None),
                    "rain": _as_int(rain[index]) if isinstance(rain, list) and index < len(rain) else 0,
                }
            )
        if not out_days:
            raise WeatherError("upstream_payload")

        current = payload.get("current") if isinstance(payload.get("current"), dict) else {}
        return {
            "city": place.city,
            "current": {
                "t": _as_int(current.get("temperature_2m")),
                "w": category_for(current.get("weather_code")),
            },
            "days": out_days,
        }

    async def get(self, address: str, days: int = 5) -> dict:
        place = await self.place_for(address)
        key = place.city.lower() or f"{place.lat:.2f},{place.lon:.2f}"
        now = time.time()

        hit = self.cache.get(key)
        if hit is not None and now - hit[0] < self.settings.weather_ttl_s:
            self.cache.move_to_end(key)
            return {**hit[1], "stamp": int(hit[0]), "stale": False}

        lock = self._inflight.get(key)
        if lock is None:
            import asyncio

            lock = asyncio.Lock()
            self._inflight[key] = lock
        async with lock:  # type: ignore[union-attr]
            # Re-check inside the lock: a second device in the same city was
            # waiting on this refresh, not queueing its own upstream call.
            hit = self.cache.get(key)
            now = time.time()
            if hit is not None and now - hit[0] < self.settings.weather_ttl_s:
                self.cache.move_to_end(key)
                return {**hit[1], "stamp": int(hit[0]), "stale": False}
            try:
                payload = await self._fetch_forecast(place, days)
            except WeatherError:
                if hit is not None and now - hit[0] < self.settings.weather_stale_s:
                    log.warning("weather upstream failed; serving stale", extra={"city": key})
                    return {**hit[1], "stamp": int(hit[0]), "stale": True}
                raise
            self.cache[key] = (now, payload)
            self.cache.move_to_end(key)
            while len(self.cache) > MAX_CACHE_KEYS:
                self.cache.popitem(last=False)
            return {**payload, "stamp": int(now), "stale": False}
