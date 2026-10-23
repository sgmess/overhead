[← Overhead](../README.md)

# Where the data comes from

Everything Overhead shows beyond the feed's positions, and how each source
is used: what is asked for, when, and what it costs. Traffic itself comes from
[adsb.fi](https://adsb.fi), or [adsb.lol](https://adsb.lol) when a request
fails; neither needs a key.

## Routes, owners, photos, airlines and flags

Routes are looked up only for the aircraft in the card or the big view, the
answers kept for the last 32 callsigns, each sent with the aircraft's
position so adsb.im can say
whether the route fits (a "?" after it when not). How far along it is
comes from the airports' positions in the same answer: the share of the leg
it is on that it has flown, great circle, and in the big view the miles to
go.

The owner is the feed's `ownOp`, which only US registrations have (from the
FAA's register, with the `year` it was built), or else
[adsbdb.com](https://www.adsbdb.com)'s registered owner, looked up by ICAO
address for the aircraft on show. The photo in the big view is the first
of planespotters.net's photos of the registration, credited to its
photographer as their terms ask, fetched once and scaled to 150 pixels high.
planespotters.net serves progressive JPEGs, which neither the P4's hardware
decoder nor LVGL's TJPGD can read, so `main/third_party/stb_image.h` decodes
them. Both are looked up one aircraft at a time and the last 16 are kept.

The flight number is shown only for a callsign of an airline's code and
digits, BAW189 as BA189, which is usually the flight number; alphanumeric
callsigns (BAW22B, EZY45GT) are assigned to avoid confusion on the radio and
aren't, so they get none. The airline comes from the callsign's first three letters (`main/core/airlines.c`, the
carriers most often seen over Europe and the large ones elsewhere), and the
country from the block of ICAO addresses each state is allocated
(`main/core/countries.c`); an aircraft that isn't an airliner gets its country's
name in place of the airline. The flags are Twemoji's, halved to 36x26 and
kept in `main/ui/flags.bin`, which `tools/make_flags.py` rebuilds after a
country is added. The airline logos are pics.avs.io's, drawn on white,
trimmed and fitted to 138x40, and shown on a white badge because many are
dark and would vanish on the card; `tools/make_logos.py` rebuilds
`main/ui/logos.bin` after an airline is added (about 1 MB for 133).

## Flight details

With an [AeroDataBox](https://aerodatabox.com) API key entered on the
configuration page, the aircraft you tap gets its flight's times and the
aircraft's details. The key is for AeroDataBox's own direct API
(`api.aerodatabox.com`, header `X-Api-Key`), not RapidAPI or API.Market,
and can run on the credits AeroDataBox gives for feeding it ADS-B.

Every answer costs credits, so only the aircraft someone picked is looked
up, never the lists or the big view stepping to the nearest:

- The flight: today's flights of that aircraft (by ICAO address), 2
  credits. That finds the flight once AeroDataBox has seen it flying. If
  none of them is about now, 2 more for today's flights by the flight
  number the callsign is (AAL91 is AA91), which the schedules are filed
  under, or for a callsign that isn't one (BAW21V), by the callsign, which
  AeroDataBox only links to a flight once it has seen it. With no date it
  answers for the nearest day with one, which can be yesterday's, so the
  date is always given. Of what comes back, only flights within three hours
  of now count; of those, the one in the air that fits where the aircraft
  is wins. An answer is kept for that aircraft and that day only, since a
  callsign is flown again the next day, often by another aircraft. Its
  times are asked for again after 15 minutes if it is still being looked
  at, or 3 hours once it has landed. "Departed" comes from the flight's
  status, not its runway time, which can be an estimate.
- The aircraft, 1 credit, kept until a restart.

The configuration page sets the most to spend a day (100 credits unless
changed) and shows what has been spent. The count is stored in flash (NVS,
its own namespace), so restarts, power cuts and a reset to the defaults
don't start the day again. Each request's cost is stored before it is
sent; if it can't be stored, nothing is asked. It is given back only when
the request can't have been charged: no connection could be made, or the
answer refused it (401, 403, 429). An answer cut off part way, a timeout
once sent or a server error may have been charged, so those stay counted. One HTTPS connection serves a tap's requests, kept until 10 s pass
without one, which saves about 1.2 s on each after the first (see
[PERFORMANCE.md](PERFORMANCE.md)). Nothing is looked up before the clock is set, a
refused key isn't tried again until another is entered, and "too many
requests" holds off for 10 minutes. Without a key, adsb.im's routes and the
feed's details are shown as before.

## Weather

`main/sources/weather.c` fetches the METAR from
[aviationweather.gov](https://aviationweather.gov)'s API every 15 minutes,
and at once when the station or the centre changes: the station set on the
configuration page, or else the one nearest the centre among those within
about 24 NM, then 72 NM. The Tab5 shows it in the radar's bottom-left
corner, the 3.4C under the title of its list view, and the page shows the
METAR itself. The runway in use comes from the traffic: aircraft within
6 NM of the station and below 3,000 ft above it, at 80 to 250 kt, climbing
or descending at 300 fpm or more, one sample each over the last 20 minutes.
When at least two agree closely, their mean track gives the runway number.
It is a true track, where runways are named by magnetic heading, so it can
be one out where the variation is large.

## Airspace and airfields

With an [OpenAIP](https://www.openaip.net) API key entered on the
configuration page, the scope also shows airspace outlines and airfields
within 107 NM of the centre, under the traffic. The key is free: create one
on openaip.net under your profile's API clients. The airspace shown is CTR,
ATZ and MATZ (blue), TMA and CTA (darker blue), restricted and prohibited
areas (red), danger areas (orange), and RMZ and TMZ (purple). FIRs, airways
and sectors are left out. Airports are always shown, labelled at 50 NM and
below. Small airfields, glider and microlight sites appear at 25 NM and
below, labelled at 10 NM. Each is drawn as a ring crossed by its main runway.
At 50 NM and below, each airport's main runway also gets an extended
centreline both ways, a dash a mile from 1 to 9.5 NM. OpenAIP gives the
runway's heading but not its thresholds, so the line runs through the
airport's reference point: at Heathrow, between the two parallel runways.
Either layer can be turned off.

The data is fetched a country at a time and trimmed to the range, because
OpenAIP's area queries time out (HTTP 408) unless they fill a whole page.
Measured 2026-10-07: GB is 800 airspaces and 415 airfields in 6 pages, which
takes about 75 s with the 10 s gaps its rate limiter needs. It runs in its
own task, so the traffic keeps updating, and again daily. The country is the
nearest airfield's unless the page lists some (for a centre near a border,
say `FR,CH`).

So that this minute and a half isn't spent at every boot, each fetched map
is kept in the `aipcache` partition (185 KB for GB at 107 NM) and drawn from
there 4.6 s after power-on, before WiFi is even up. It is fetched again in
the background once it is a day old, or at once when the centre or the
countries change. The partition was added after release, in flash the app
slots leave free; an update over WiFi can't add a partition, so a device
only ever updated that way fetches at every boot as before, until one USB
flash of the partition table (`idf.py -B build_tab5 partition-table-flash`,
which keeps the saved settings). The web flasher's full image has it.
