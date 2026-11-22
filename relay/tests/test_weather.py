"""Calendar weather: IP resolution, geocoding order, caching and the route.

No test touches the network. ``_get_text`` is the single seam every upstream
call goes through (``_get_json`` parses it), so replacing it gives a deterministic
table of canned payloads, failures and slowdowns.
"""

from __future__ import annotations

import asyncio
import json

import pytest
from fastapi.testclient import TestClient

from relay import weather
from relay.config import get_settings, reload_settings
from relay.main import app

HEADERS = {
    "Authorization": "Bearer test-device-token",
    "X-Device-Id": "debug-client",
    "X-Voice-Protocol": "1",
}

# A real public address: 203.0.113.0/24 is TEST-NET-3 and Python reports it as
# private, which the resolver deliberately treats as unlocatable.
CLIENT_IP = "223.5.5.5"

IPSB_OK = {"ip": CLIENT_IP, "city": "常州", "region": "Jiangsu",
           "latitude": 31.7736, "longitude": 119.9540}
IPINFO_OK = {"ip": CLIENT_IP, "city": "南京", "region": "Jiangsu", "loc": "32.0603,118.7969"}
IPAPI_OK = {"status": "success", "city": "苏州", "regionName": "江苏省", "lat": 31.2990, "lon": 120.5853}
IPAPI_FAIL = {"status": "fail", "message": "reserved range"}
OPEN_METEO_OK = {
    "current": {"temperature_2m": 21.4, "weather_code": 3},
    "daily": {
        "time": ["2026-11-16", "2026-11-17", "2026-11-18", "2026-11-19", "2026-11-20"],
        "temperature_2m_max": [21.2, 20.0, 18.4, 19.0, 22.0],
        "temperature_2m_min": [13.4, 12.2, 11.0, 10.4, 13.1],
        "weather_code": [2, 3, 61, 45, 0],
        "precipitation_probability_max": [10, 20, 80, 30, 5],
    },
}

IPSB_GEO = "https://api.ip.sb/geoip/"
IPSB_IP = "https://api.ip.sb/ip"
IPINFO_GEO = "https://ipinfo.io/"
IPAPI_GEO = "http://ip-api.com/json/"
OPEN_METEO = "https://api.open-meteo.com/v1/forecast"


class FakeUpstream:
    """Canned responses keyed by URL prefix; records every call made."""

    def __init__(self, responses: dict[str, object]):
        self.responses = responses
        self.calls: list[str] = []

    async def __call__(self, url: str, params=None, timeout=None):
        self.calls.append(url)
        for prefix, payload in self.responses.items():
            if url.startswith(prefix):
                if isinstance(payload, Exception):
                    raise payload
                return payload if isinstance(payload, str) else json.dumps(payload)
        raise weather.WeatherError("upstream_unavailable")


# ------------------------------------------------------------------ pure helpers


def test_resolve_client_ip_prefers_forwarded_chain():
    assert weather.resolve_client_ip("1.1.1.1, 10.0.0.1", "127.0.0.1") == "1.1.1.1"
    assert weather.resolve_client_ip("1.1.1.1", None) == "1.1.1.1"
    # A proxy that appends nothing still must not yield an empty first hop.
    assert weather.resolve_client_ip(", 1.1.1.1", "127.0.0.1") == "1.1.1.1"
    assert weather.resolve_client_ip(None, "8.8.4.4") == "8.8.4.4"
    assert weather.resolve_client_ip("", "") == ""


@pytest.mark.parametrize(
    "host",
    ["127.0.0.1", "::1", "10.1.2.3", "192.168.1.20", "172.16.0.5", "169.254.1.1", "0.0.0.0", "testclient", ""],
)
def test_private_addresses_are_not_geocoded(host):
    assert weather.is_private_address(host) is True


@pytest.mark.parametrize("host", ["223.5.5.5", "8.8.4.4", "2400:3200::1"])
def test_public_addresses_are_geocodable(host):
    assert weather.is_private_address(host) is False


def test_weather_codes_map_to_the_eight_categories():
    assert weather.category_for(0) == 0
    assert weather.category_for(2) == 1
    assert weather.category_for(3) == 2
    assert weather.category_for(48) == 3
    assert weather.category_for(61) == 4
    assert weather.category_for(65) == 5
    assert weather.category_for(75) == 6
    assert weather.category_for(95) == 7
    # Unknown / missing codes read as cloudy rather than dropping the line.
    assert weather.category_for(None) == weather.UNKNOWN_CATEGORY
    assert weather.category_for("nonsense") == weather.UNKNOWN_CATEGORY


# ------------------------------------------------------------------------- store


def _store(responses: dict[str, object], monkeypatch) -> tuple[weather.WeatherStore, FakeUpstream]:
    fake = FakeUpstream(responses)
    monkeypatch.setattr(weather.WeatherStore, "_get_text", fake)
    return weather.WeatherStore(get_settings()), fake


def test_forecast_uses_the_geocoded_city(monkeypatch):
    store, fake = _store({IPSB_GEO: IPSB_OK, OPEN_METEO: OPEN_METEO_OK}, monkeypatch)
    payload = asyncio.run(store.get(CLIENT_IP, 5))

    assert payload["city"] == "常州"
    assert payload["stale"] is False
    assert payload["current"] == {"t": 21, "w": 2}
    assert [day["w"] for day in payload["days"]] == [1, 2, 4, 3, 0]
    assert payload["days"][0] == {"d": "2026-11-16", "hi": 21, "lo": 13, "w": 1, "rain": 10}
    assert payload["days"][2]["hi"] == 18  # 18.4 rounds, it does not truncate
    assert fake.calls[0].startswith(IPSB_GEO)


def test_geocoders_are_tried_in_reachability_order(monkeypatch):
    # ip.sb does not answer from the production host? It does — but when it
    # fails the next provider must be ipinfo, not the unreachable ip-api.
    store, fake = _store(
        {IPSB_GEO: weather.WeatherError("upstream_unavailable"), IPINFO_GEO: IPINFO_OK, OPEN_METEO: OPEN_METEO_OK},
        monkeypatch,
    )
    payload = asyncio.run(store.get(CLIENT_IP, 5))
    assert payload["city"] == "南京"
    assert [call.split("//")[1].split("/")[0] for call in fake.calls[:2]] == ["api.ip.sb", "ipinfo.io"]


def test_ipapi_is_the_last_resort(monkeypatch):
    store, _ = _store(
        {
            IPSB_GEO: {"ip": CLIENT_IP},  # no city and no coordinates
            IPINFO_GEO: {"ip": CLIENT_IP},
            IPAPI_GEO: IPAPI_OK,
            OPEN_METEO: OPEN_METEO_OK,
        },
        monkeypatch,
    )
    assert asyncio.run(store.get(CLIENT_IP, 5))["city"] == "苏州"


def test_private_caller_falls_back_to_the_relay_address(monkeypatch):
    store, fake = _store({IPSB_IP: CLIENT_IP, IPSB_GEO: IPSB_OK, OPEN_METEO: OPEN_METEO_OK}, monkeypatch)
    payload = asyncio.run(store.get("192.168.1.20", 5))
    assert payload["city"] == "常州"
    assert fake.calls[0] == IPSB_IP


def test_the_relay_address_falls_back_to_ipinfo(monkeypatch):
    store, fake = _store(
        {
            IPSB_IP: weather.WeatherError("upstream_unavailable"),
            "https://ipinfo.io/json": {"ip": CLIENT_IP},
            IPSB_GEO: IPSB_OK,
            OPEN_METEO: OPEN_METEO_OK,
        },
        monkeypatch,
    )
    assert asyncio.run(store.get("127.0.0.1", 5))["city"] == "常州"
    assert fake.calls[1] == "https://ipinfo.io/json"


def test_every_geocoder_failing_is_a_geo_error(monkeypatch):
    store, _ = _store(
        {
            IPSB_GEO: weather.WeatherError("upstream_status"),
            IPINFO_GEO: {"ip": CLIENT_IP},
            IPAPI_GEO: IPAPI_FAIL,
        },
        monkeypatch,
    )
    with pytest.raises(weather.WeatherError) as excinfo:
        asyncio.run(store.get(CLIENT_IP, 5))
    assert excinfo.value.code == "geo_failed"


def test_an_egress_lookup_failure_is_a_geo_error(monkeypatch):
    store, _ = _store(
        {
            IPSB_IP: weather.WeatherError("upstream_unavailable"),
            "https://ipinfo.io/json": weather.WeatherError("upstream_unavailable"),
        },
        monkeypatch,
    )
    with pytest.raises(weather.WeatherError) as excinfo:
        asyncio.run(store.get("192.168.1.20", 5))
    assert excinfo.value.code == "geo_failed"


def test_a_fresh_forecast_is_served_from_cache(monkeypatch):
    store, fake = _store({IPSB_GEO: IPSB_OK, OPEN_METEO: OPEN_METEO_OK}, monkeypatch)
    asyncio.run(store.get(CLIENT_IP, 5))
    asyncio.run(store.get(CLIENT_IP, 5))
    forecasts = sum(1 for call in fake.calls if call.startswith(OPEN_METEO))
    assert forecasts == 1


def test_an_upstream_failure_serves_the_previous_forecast_as_stale(monkeypatch):
    store, fake = _store({IPSB_GEO: IPSB_OK, OPEN_METEO: OPEN_METEO_OK}, monkeypatch)
    asyncio.run(store.get(CLIENT_IP, 5))

    # Age the cache past the TTL but keep it inside the stale window.
    for key, (stamp, payload) in list(store.cache.items()):
        store.cache[key] = (stamp - get_settings().weather_ttl_s - 1, payload)
    fake.responses[OPEN_METEO] = weather.WeatherError("upstream_unavailable")

    payload = asyncio.run(store.get(CLIENT_IP, 5))
    assert payload["stale"] is True
    assert payload["city"] == "常州"


def test_a_pinned_location_skips_geocoding(monkeypatch):
    monkeypatch.setenv("WEATHER_LAT", "31.7736")
    monkeypatch.setenv("WEATHER_LON", "119.9540")
    monkeypatch.setenv("WEATHER_CITY", "常州")
    reload_settings()
    try:
        store, fake = _store({OPEN_METEO: OPEN_METEO_OK}, monkeypatch)
        payload = asyncio.run(store.get(CLIENT_IP, 5))
        assert payload["city"] == "常州"
        assert not any("ip.sb" in call or "ipinfo" in call or "ip-api" in call for call in fake.calls)
    finally:
        monkeypatch.delenv("WEATHER_LAT", raising=False)
        monkeypatch.delenv("WEATHER_LON", raising=False)
        monkeypatch.delenv("WEATHER_CITY", raising=False)
        reload_settings()


# ------------------------------------------------------------------------- route


@pytest.fixture()
def client():
    reload_settings()
    with TestClient(app) as test_client:
        yield test_client


def test_route_requires_a_device_token(client):
    assert client.get("/api/v1/weather").status_code == 401


def test_route_returns_the_forecast(client, monkeypatch):
    fake = FakeUpstream({IPSB_IP: CLIENT_IP, IPSB_GEO: IPSB_OK, OPEN_METEO: OPEN_METEO_OK})
    monkeypatch.setattr(weather.WeatherStore, "_get_text", fake)

    response = client.get("/api/v1/weather", headers=HEADERS)
    assert response.status_code == 200
    body = response.json()
    assert body["ok"] is True
    assert body["city"] == "常州"
    assert len(body["days"]) == 5
    assert body["days"][0]["d"] == "2026-11-16"
    # TestClient reports the peer as "testclient", which is not an address: the
    # relay must resolve its own egress address instead of geocoding garbage.
    assert fake.calls[0] == IPSB_IP


def test_route_reports_a_geo_failure_as_a_502(client, monkeypatch):
    fake = FakeUpstream(
        {
            IPSB_IP: CLIENT_IP,
            IPSB_GEO: weather.WeatherError("upstream_unavailable"),
            IPINFO_GEO: weather.WeatherError("upstream_unavailable"),
            IPAPI_GEO: weather.WeatherError("upstream_unavailable"),
        }
    )
    monkeypatch.setattr(weather.WeatherStore, "_get_text", fake)

    response = client.get("/api/v1/weather", headers=HEADERS)
    assert response.status_code == 502
    assert response.json() == {"ok": False, "error": "geo_failed"}


def test_route_days_parameter_is_clamped_to_the_configured_maximum(client, monkeypatch):
    fake = FakeUpstream({IPSB_IP: CLIENT_IP, IPSB_GEO: IPSB_OK, OPEN_METEO: OPEN_METEO_OK})
    monkeypatch.setattr(weather.WeatherStore, "_get_text", fake)

    body = client.get("/api/v1/weather", params={"days": 99}, headers=HEADERS).json()
    assert len(body["days"]) == get_settings().weather_days
