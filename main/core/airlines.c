#include "airlines.h"

#include <stdio.h>
#include <string.h>

// In ICAO code order, for the binary search
static const airline_t AIRLINES[] = {
    {"AAL", "AA", "American Airlines"},
    {"AAR", "OZ", "Asiana Airlines"},
    {"ABW", "RU", "AirBridgeCargo"},
    {"ABY", "G9", "Air Arabia"},
    {"ACA", "AC", "Air Canada"},
    {"AEA", "UX", "Air Europa"},
    {"AEE", "A3", "Aegean Airlines"},
    {"AFL", "SU", "Aeroflot"},
    {"AFR", "AF", "Air France"},
    {"AHY", "J2", "Azerbaijan Airlines"},
    {"AIC", "AI", "Air India"},
    {"ALK", "UL", "SriLankan Airlines"},
    {"AMX", "AM", "Aeromexico"},
    {"ANA", "NH", "All Nippon Airways"},
    {"ANE", "YW", "Air Nostrum"},
    {"ANZ", "NZ", "Air New Zealand"},
    {"ARG", "AR", "Aerolineas Argentinas"},
    {"ASA", "AS", "Alaska Airlines"},
    {"ASL", "JU", "Air Serbia"},
    {"AUA", "OS", "Austrian Airlines"},
    {"AUR", "GR", "Aurigny"},
    {"AVA", "AV", "Avianca"},
    {"AXM", "AK", "AirAsia"},
    {"AZU", "AD", "Azul"},
    {"BAW", "BA", "British Airways"},
    {"BCS", "QY", "European Air Transport"},
    {"BCY", "WX", "CityJet"},
    {"BEL", "SN", "Brussels Airlines"},
    {"BTI", "BT", "airBaltic"},
    {"CAL", "CI", "China Airlines"},
    {"CCA", "CA", "Air China"},
    {"CES", "MU", "China Eastern"},
    {"CFE", "BA", "BA CityFlyer"},
    {"CFG", "DE", "Condor"},
    {"CKS", "K4", "Kalitta Air"},
    {"CLH", "LH", "Lufthansa CityLine"},
    {"CLX", "CV", "Cargolux"},
    {"CMP", "CM", "Copa Airlines"},
    {"CPA", "CX", "Cathay Pacific"},
    {"CSA", "OK", "Czech Airlines"},
    {"CSN", "CZ", "China Southern"},
    {"CTN", "OU", "Croatia Airlines"},
    {"DAL", "DL", "Delta Air Lines"},
    {"DHK", "D0", "DHL Air"},
    {"DLH", "LH", "Lufthansa"},
    {"EDW", "WK", "Edelweiss"},
    {"EFW", "BA", "BA Euroflyer"},
    {"EIN", "EI", "Aer Lingus"},
    {"EJA", "", "NetJets"},
    {"EJU", "EC", "easyJet Europe"},
    {"ELY", "LY", "El Al"},
    {"ETD", "EY", "Etihad Airways"},
    {"ETH", "ET", "Ethiopian Airlines"},
    {"EUK", "EI", "Aer Lingus UK"},
    {"EVA", "BR", "EVA Air"},
    {"EWG", "EW", "Eurowings"},
    {"EXS", "LS", "Jet2"},
    {"EZS", "DS", "easyJet Switzerland"},
    {"EZY", "U2", "easyJet"},
    {"FDB", "FZ", "flydubai"},
    {"FDX", "FX", "FedEx"},
    {"FFT", "F9", "Frontier Airlines"},
    {"FIN", "AY", "Finnair"},
    {"GFA", "GF", "Gulf Air"},
    {"GIA", "GA", "Garuda Indonesia"},
    {"GLO", "G3", "Gol"},
    {"GTI", "5Y", "Atlas Air"},
    {"HAL", "HA", "Hawaiian Airlines"},
    {"HOP", "A5", "HOP!"},
    {"HVN", "VN", "Vietnam Airlines"},
    {"IBE", "IB", "Iberia"},
    {"IBS", "I2", "Iberia Express"},
    {"ICE", "FI", "Icelandair"},
    {"IGO", "6E", "IndiGo"},
    {"ITY", "AZ", "ITA Airways"},
    {"JAF", "TB", "TUI fly Belgium"},
    {"JAL", "JL", "Japan Airlines"},
    {"JBU", "B6", "JetBlue"},
    {"JST", "JQ", "Jetstar"},
    {"KAC", "KU", "Kuwait Airways"},
    {"KAL", "KE", "Korean Air"},
    {"KLC", "WA", "KLM Cityhopper"},
    {"KLM", "KL", "KLM"},
    {"KQA", "KQ", "Kenya Airways"},
    {"KZR", "KC", "Air Astana"},
    {"LAN", "LA", "LATAM Airlines"},
    {"LGL", "LG", "Luxair"},
    {"LOG", "LM", "Loganair"},
    {"LOT", "LO", "LOT Polish Airlines"},
    {"MAS", "MH", "Malaysia Airlines"},
    {"MEA", "ME", "Middle East Airlines"},
    {"MSR", "MS", "EgyptAir"},
    {"NAX", "DY", "Norwegian"},
    {"NJE", "", "NetJets Europe"},
    {"NKS", "NK", "Spirit Airlines"},
    {"NSZ", "D8", "Norwegian"},
    {"OMA", "WY", "Oman Air"},
    {"PAL", "PR", "Philippine Airlines"},
    {"PGT", "PC", "Pegasus"},
    {"PIA", "PK", "Pakistan International"},
    {"QFA", "QF", "Qantas"},
    {"QTR", "QR", "Qatar Airways"},
    {"RAM", "AT", "Royal Air Maroc"},
    {"RJA", "RJ", "Royal Jordanian"},
    {"ROT", "RO", "TAROM"},
    {"RUK", "RK", "Ryanair UK"},
    {"RYR", "FR", "Ryanair"},
    {"SAA", "SA", "South African Airways"},
    {"SAS", "SK", "SAS"},
    {"SHT", "", "British Airways"}, // the Shuttle: SHT8 is not BA8
    {"SIA", "SQ", "Singapore Airlines"},
    {"SVA", "SV", "Saudia"},
    {"SWA", "WN", "Southwest Airlines"},
    {"SWR", "LX", "Swiss"},
    {"SXS", "XQ", "SunExpress"},
    {"TAM", "JJ", "LATAM Brasil"},
    {"TAP", "TP", "TAP Air Portugal"},
    {"TFL", "OR", "TUI fly Netherlands"},
    {"TGW", "TR", "Scoot"},
    {"THA", "TG", "Thai Airways"},
    {"THY", "TK", "Turkish Airlines"},
    {"TOM", "BY", "TUI Airways"},
    {"TRA", "HV", "Transavia"},
    {"TSC", "TS", "Air Transat"},
    {"TUI", "X3", "TUIfly"},
    {"TVF", "TO", "Transavia France"},
    {"UAE", "EK", "Emirates"},
    {"UAL", "UA", "United Airlines"},
    {"UPS", "5X", "UPS Airlines"},
    {"UZB", "HY", "Uzbekistan Airways"},
    {"VIR", "VS", "Virgin Atlantic"},
    {"VJT", "", "VistaJet"},
    {"VLG", "VY", "Vueling"},
    {"VOE", "V7", "Volotea"},
    {"VOZ", "VA", "Virgin Australia"},
    {"WJA", "WS", "WestJet"},
    {"WMT", "5W", "Wizz Air Malta"},
    {"WUK", "W9", "Wizz Air UK"},
    {"WZZ", "W6", "Wizz Air"},
};

const airline_t *airline_find(const char *icao)
{
    int lo = 0, hi = sizeof(AIRLINES) / sizeof(AIRLINES[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strncmp(icao, AIRLINES[mid].icao, 3);
        if (c < 0) {
            hi = mid - 1;
        } else if (c > 0) {
            lo = mid + 1;
        } else {
            return strlen(icao) == 3 ? &AIRLINES[mid] : NULL;
        }
    }
    return NULL;
}

const airline_t *airline_for_callsign(const char *callsign)
{
    for (int i = 0; i < 3; i++) {
        if (callsign[i] < 'A' || callsign[i] > 'Z') return NULL;
    }
    if (callsign[3] < '0' || callsign[3] > '9') return NULL;
    char icao[4] = {callsign[0], callsign[1], callsign[2], '\0'};
    return airline_find(icao);
}

bool airline_flight_number(const char *callsign, char *out, size_t n)
{
    const airline_t *al = airline_for_callsign(callsign);
    if (!al || !al->iata[0]) return false;
    const char *num = callsign + 3;
    size_t len = strlen(num);
    if (len < 1 || len > 4 || strspn(num, "0123456789") != len) return false;
    while (num[0] == '0' && num[1]) num++; // BAW0001 is BA1
    snprintf(out, n, "%s%s", al->iata, num);
    return true;
}
