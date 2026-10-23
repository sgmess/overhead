# Overhead

<p align="center">
  <a href="https://sgmess.github.io/overhead/"><img alt="Flash in Browser" src="https://img.shields.io/badge/Flash%20in%20Browser-FF6D00?logo=googlechrome&logoColor=white"></a>
  <img alt="Boards" src="https://img.shields.io/badge/Boards-ESP32--P4%20%C2%B7%20M5Stack--Tab5-E7352C?logo=espressif&logoColor=white">
  <a href="https://github.com/sgmess/overhead/releases"><img alt="GitHub Release" src="https://img.shields.io/github/v/release/sgmess/overhead?color=7B42BC"></a>
  <a href="LICENSE"><img alt="GitHub License" src="https://img.shields.io/github/license/sgmess/overhead"></a>
  <img alt="GitHub top language" src="https://img.shields.io/github/languages/top/sgmess/overhead">
</p>

A small, single-screen ADS-B radar for ESP32-P4 touch displays. It joins
WiFi, polls a free online ADS-B feed, and plots the aircraft around a fixed
point on a radar scope.

| Board | `OVERHEAD_BOARD` | Panel |
| --- | --- | --- |
| ESP32-P4-WIFI6-Touch-LCD-3.4C | `p4_34c` (default) | 800x800 MIPI DSI, WiFi via ESP32-C6 |
| M5Stack Tab5 | `tab5` | 720x1280 MIPI DSI portrait, WiFi via ESP32-C6 |

- Aircraft drawn as arrowheads along their track, coloured by altitude (amber
  low, lime, cyan, blue, violet high), with an optional line to where each
  will be in a minute.
- Radar-style data tags: callsign, altitude in hundreds of feet, and a climb or
  descent arrow, with type and ground speed as options. The nearest aircraft
  get labelled first.
- Trails behind the nearest 50 aircraft, fading with age (1 to 10 minutes).
- Helicopters drawn with a rotor and tail boom; military aircraft (the feed's
  `dbFlags`) in a pink diamond.
- Origin and destination in the aircraft card, from
  [adsb.im](https://adsb.im)'s route database (the one tar1090 uses), with
  the flight number, the airline's name and logo, and the flag of the
  country the aircraft is registered in, all worked out on the device; how
  far along its route it is ("LHR > JFK • 23%"); its registered owner,
  when that isn't the airline, and for US registrations the year it was
  built; and in the big view a photo from [planespotters.net](https://www.planespotters.net).
- With an [AeroDataBox](https://aerodatabox.com) key (optional): for the
  aircraft you pick, when its flight left and gets in and how late it is
  ("Dep 14:23  Arr 00:24 • on time"), the flight number it's sold under,
  today's route, the terminal and gate it arrives at, and the aircraft's
  full type, age and seats ("Airbus A380-800", "built 2019, 7 years old •
  516 seats").
- An overhead alert: an aircraft within a set distance and below a set
  altitude blinks amber, a banner names it, and the Tab5 or 3.4C chimes.
- An emergency alert: any aircraft in range squawking 7700, 7600 or 7500, or
  reporting an emergency over ADS-B (medical, low fuel), gets a red banner
  ("7700 EMERGENCY BAW123 12.3 NM") ahead of any overhead alert, and a chime.
- A watchlist: types, registrations, callsigns or addresses ("A388, G-XLEA,
  RRR*"), and if wanted every military aircraft or any type never seen here
  before. One in range gets a violet ring and banner, and a chime.
- Where to look: the card and the big view give the compass point and the
  height above the horizon ("13.8 NM SE ▲5°"), allowing for the Earth's curve;
  a setting shows the bearing in both instead, as before.
- The weather at the airfield nearest the centre, or one chosen, from its
  METAR (wind, temperature, pressure), and the runway in use there, from
  which way the traffic low over it is landing and taking off.
- A filter by kind of aircraft (airliners, light aircraft, helicopters,
  other, from the transponder's emitter category) and altitude band. The
  count then reads "12 of 30 aircraft"; emergencies always show.
- Brightness, with dimming from sunset to sunrise at the radar centre or
  between set times, and the screen fully off overnight if wanted: a touch
  wakes it (and does nothing else), and it goes off again two minutes later.
- Traffic stats on the configuration page: today's aircraft seen, military,
  the busiest hour, the highest and fastest, the top airlines and types (by
  name, "Boeing 787-8 Dreamliner"), the rarest types here and a bar chart by
  hour; every aircraft that passed
  overhead, at its closest, for today, the last week or month or all kept
  (about 16,000), with a CSV download; and over the days, the average day of
  the last week, the most in a day and the running total, kept through restarts.
- Positions are moved forward between polls using ground speed and track, so
  traffic moves smoothly instead of jumping every few seconds.
- Emergency squawks (7500/7600/7700) are drawn red and circled.
- Extended runway centrelines at the airports, a dash a mile out to 9.5 NM,
  where arrivals line up and departures climb out.
- UTC or local clock (or both), a sweep turning every 8 s, compass rose,
  range rings.
  On the Tab5 the clock, range, traffic count and battery sit in the corners
  around the scope; the round 3.4C has no corners, so it keeps them along
  the top and bottom of the scope.
- Battery level, charging state and time left on the Tab5, with charging
  enabled and a clean power-off before the pack runs flat.

Data comes from [adsb.fi](https://adsb.fi), with an automatic switch to
[adsb.lol](https://adsb.lol) if a request fails; either can be made the
primary in the settings. Neither needs an API key.

## Touch

At power-up a start-up screen shows the radar in miniature, what start-up is
doing (joining WiFi, then receiving traffic) and the firmware version, with an
arc counting down 10 s. A tap dismisses it sooner, and does nothing else. It
gives way at once to the WiFi setup portal.

| Tap | Does |
| --- | --- |
| An aircraft | Opens its card: flag, flight number, airline and logo, registration, type, route, altitude, V/S, speed, track, squawk, range and bearing |
| The alert banner | Opens the card of the nearest aircraft that set it off |
| The card | Opens that aircraft in the big view |
| Empty scope, card open | Closes the card |
| Swipe up or down on the scope | Up or down a range: 5, 10, 25, 50, 100 NM |
| A list row (Tab5) | Selects that aircraft (in the big view too, with the big view up); tap it again to clear |
| Long press on the scope | Opens the settings screen |
| Swipe left or right | The next view, or the one before: scope, big view, and on the round 3.4C a list |
| The big view | Selects the next nearest aircraft, back round to the nearest |
| A row of the 3.4C's list | Selects that aircraft and opens it in the big view |

The big view shows one aircraft in large type, for across the room: flag and
logo, callsign, flight number and airline, altitude in its scope colour with
which way it is going, speed with an arrow along its track (north up, as on
the scope), then distance, bearing and squawk, the route with the airports'
towns ("SDF > CGN", "Louisville > Cologne") and how far along it is,
registration and type, the owner and age, and a photo. The card
only shows with the scope, and whatever the big view or a list picks stays
selected there when you swipe back. An aircraft that has just started an
emergency or come overhead takes the big view over until it stops or a tap
moves on; otherwise it shows the selection, or with none the nearest airborne
(not counting aircraft sitting on the ground without saying so: no altitude
and under 50 kt). The nearest stays shown until another is half a mile
nearer, so a near tie doesn't flicker. On the Tab5 it fills
the scope's square and the list stays beside it. Its route is looked up as
the card's is, once per callsign.

The round 3.4C has no room for the Tab5's list beside the scope, so
it gets it as a third view: the nearest 10 in the middle of
the circle, with the same columns, colours and count.

## Install

From a browser, with the board on USB: [the web flasher](https://sgmess.github.io/overhead/)
(Chrome or Edge on a computer) installs the latest release and can set up its
WiFi. The [releases](https://github.com/sgmess/overhead/releases) also have each
board's whole flash image for esptool, and updates install over WiFi from the
configuration page. To build it yourself, see [docs/BUILDING.md](docs/BUILDING.md).

## Setup and settings

With no network configured, the first boot opens a setup portal: an open
access point named `Overhead-XXXX` (the last four hex digits of the MAC).
The radar shows its name and a QR code that joins it. Phones then open the
setup page by themselves (DNS answers every name with the device, and DHCP
option 114 names the page); otherwise browse to `http://192.168.4.1`. The page
lists nearby networks and takes the password and the radar centre. Saving
restarts the device onto that network. If a saved network hasn't connected
30 s after boot, the portal opens as well. The saved network is retried every
15 s meanwhile, and the portal closes once it connects.

Once connected, the configuration page is at `http://overhead.local` (mDNS),
or at the address shown on the radar until the first traffic arrives. It
covers the centre (with a map), starting range, labels and what the data tags
show, trails, sweep, ground traffic, military highlighting, routes, where to
look or the bearing, the filter, the overhead, emergency and watchlist
alerts, map layers, the weather, flight details (AeroDataBox), brightness and
night dimming, the clock and timezone, primary feed, poll interval, the
Tab5's orientation and battery cut-off, and changing or forgetting the
network. It also shows today's traffic: the statistics, the types seen, and
every aircraft that passed overhead, with a CSV download. Saving applies at once; only a new orientation or
network restarts the device. The pages are unauthenticated, so anyone on the
same network can change the settings.

A long press on the scope opens the same settings on the device itself,
with the status (network, address, uptime, memory, battery, map layers) at
the top and firmware updates (check, then install) at the bottom:
everything that needs no typing (so not the centre, the WiFi password, the
API keys, the watchlist, the weather station or a typed-in timezone, which
the screen sends you to the page for). Each change applies as it is made, and the screen closes after a minute
untouched.

Optional extras need a key entered on the page: AeroDataBox for flight times
and aircraft details, and OpenAIP for airspace and airfields. How these and
every other source are used is in [docs/DATA.md](docs/DATA.md).

## More

| Document | What is in it |
| --- | --- |
| [docs/BUILDING.md](docs/BUILDING.md) | Building and flashing, releases and the web flasher, updates over WiFi, the host tests, debug builds and the display benchmark |
| [docs/DATA.md](docs/DATA.md) | Routes, owners, photos, airlines and flags; AeroDataBox flight details and its credits; the weather and runway in use; OpenAIP airspace |
| [docs/ALERTS-AND-STATS.md](docs/ALERTS-AND-STATS.md) | Clock and dimming, the screen off overnight, the overhead, emergency and watchlist alerts, the statistics and the overhead log |
| [docs/TAB5.md](docs/TAB5.md) | The Tab5's layout and display pipeline, internal RAM, ESP-IDF 6.1, the C6 firmware, the battery |
| [docs/CODE.md](docs/CODE.md) | What each source file does, how settings are saved, how the radar is drawn |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | Every display and memory experiment, the results, and what was decided |

## Licence

Apache License 2.0; see [LICENSE](LICENSE). `boards/tab5/esp_driver_ppa/`
is ESP-IDF's, Apache-2.0, changed as its README says. `main/third_party/stb_image.h` is Sean Barrett's,
public domain or MIT. The flag images in `main/ui/flags.bin` are from
[Twemoji](https://github.com/jdecked/twemoji), Copyright 2019 Twitter, Inc
and other contributors, licensed under
[CC-BY 4.0](https://creativecommons.org/licenses/by/4.0/); they were scaled
down for the screen. The airline logos in `main/ui/logos.bin` are trademarks of
their airlines and are not covered by this licence. The licence covers this code only: aircraft data from
adsb.fi and adsb.lol, routes from adsb.im, owners from adsbdb.com, photos
from planespotters.net (each its photographer's), flight and aircraft details
from AeroDataBox, weather from aviationweather.gov, aircraft type names from
ICAO Doc 8643 by way of [tar1090-db](https://github.com/wiedehopf/tar1090-db),
airspace from OpenAIP and the
IP-location answers from ipinfo.io and ipwho.is are under those services'
own terms.
