// SPDX-License-Identifier: GPL-3.0-only
// See waste_trace.h.
#include "waste_trace.h"
#include "determinism.h"
#include <map>
#include <vector>

typedef unsigned __int64 U64;

#define FIND_WASTE_RVA 0x19E330
#define FIND_UNLOAD_RVA 0x19E750
#define WORLD_RVA 0x9D4F10
#define RESOURCES_OFFSET 0xC2B0
#define RESOURCE_BYTES 0x340
#define RESOURCE_WEIGHT 0x2EC
#define VEHICLES_OFFSET 0x12810
#define OFFICE_SITES 0xF90       // the office's buildings
#define OFFICE_VEHICLES 0xC70    // the office's vehicles
#define SITE_OFFICES 0xDA8       // the offices a building is linked to
#define SLOTS 0x970              // a building's storage slots
#define SLOT_BYTES 0xE0
#define SLOT_LIMITS 0x18         // a second list of the slot's resources, 19e330 reads through 4292d0
#define SLOT_MODE 0x88
#define SLOT_SIZE 0x8C
#define SLOT_KIND 0x90
#define SLOT_ENTRY_BYTES 0x10
#define WASTE_KIND 0x11
#define VEHICLE_ID 0x04
#define VEHICLE_IN 0x4F0
#define VEHICLE_HOME 0x4F8
#define VEHICLE_RECORDS 0x638    // one per stop, 0x198 bytes: its resources at +8
#define VEHICLE_STOPS 0x680
#define VEHICLE_STATE 0x698
#define VEHICLE_PATH 0x6A0
#define VEHICLE_PATH_AT 0x700
#define VEHICLE_POSITION 0xD90
#define VEHICLE_TYPE 0x1708
#define TYPE_CLASS 0x8600
#define RECORD_BYTES 0x198
#define RECORD_RESOURCES 0x08
#define MAX_LISTED 4096
#define NOTES 8192
#define ASKED_NOTES 16384
#define CHANGE_NOTES 8192
#define NOTES_DUMPED 130         // the games are compared every 120 ticks

typedef U64 (*FindWasteFn)(U64 world, U64 office, float capacity, U64 list, unsigned char flag, U64* resource, U64 list2);
typedef U64 (*FindUnloadFn)(U64 world, U64 vehicle, U64 resource, float radius, unsigned char flag);

namespace { // this file's own types (see save/mod_save.cpp's Entry)

struct Note {
    unsigned long long tick;
    int kind;     // 0 waste for an empty truck, 1 a place to unload
    int subject;  // kind 0 the office's id, kind 1 the truck's
    int answer;   // the building's id, -1 none
    int resource; // its place in the world's resources, -1 none
    unsigned wasteHash;
    unsigned siteHash;   // what else the search reads of the office's buildings
    unsigned listHash;   // kind 0: the two lists of resources it is given
    unsigned truckHash;  // every waste truck's job (TruckJob)
    float amount; // the waste the office's buildings hold, summed
    float x, z;   // kind 1: the truck
    float arg;    // kind 0 the truck's capacity, kind 1 the radius
    int flag;
};

// One question to 19f5f0 inside a search: is this job taken?
struct Asked {
    unsigned long long tick;
    int building;
    int resource;
    int taken;    // 0 no, 1 yes, 2 the office guard refused it
    int truck;    // the first truck 19f5f0's walk finds on the job, -1 none
};

// What 19f5f0 and the searches read of a waste truck and the state hash leaves out.
struct TruckJob {
    int id;
    int home;     // +4f8
    int in;       // +4f0
    int state;    // +698: 19f5f0 counts a truck with 0
    int at;       // +700: its place on the path +6a0
    int path;     // the path's length
    int stops;    // +680
    int firstStop;
    int records;  // +638
    int firstResource;
    unsigned stopsHash;
    unsigned recordsHash;
};

struct TruckChange {
    unsigned long long tick;
    TruckJob job;
    int gone;
};

struct Known {
    TruckJob job;
    unsigned seen;
};

} // namespace

static Note g_notes[NOTES];
static unsigned long long g_count = 0;
static Asked g_asked[ASKED_NOTES];
static unsigned long long g_askedCount = 0;
static TruckChange g_changes[CHANGE_NOTES];
static unsigned long long g_changeCount = 0;
static unsigned long long g_lastTick = 0;
static std::map<int, Known> g_known; // g_lock: a report is written on the receive thread
static unsigned long long g_knownTick = 0;
static unsigned g_snapshot = 0;
static thread_local bool t_inSearch = false;

void WasteTrace_Bind(const ModHost* host)
{
    ModHost_Bind(host);
}

static U64 Q(U64 at) { return *(U64*)at; }
static void Mix(unsigned& h, unsigned v) { h = (h ^ v) * 16777619u; }

static int BuildingId(U64 building)
{
    __try {
        return building ? *(int*)building : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

static int ResourceIndex(U64 resource)
{
    __try {
        U64 first = Q((U64)(g_exeBase + WORLD_RVA + RESOURCES_OFFSET));
        if (!resource || resource < first || (resource - first) % RESOURCE_BYTES) return -1;
        return (int)((resource - first) / RESOURCE_BYTES);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

// What the search reads: every waste slot's entries of the office's buildings, to the bit.
static void HashWaste(U64 office, unsigned* hash, float* amount)
{
    unsigned h = 2166136261u;
    float sum = 0;
    __try {
        U64 b = office ? Q(office + OFFICE_SITES) : 0, e = office ? Q(office + OFFICE_SITES + 8) : 0;
        for (unsigned i = 0; b && b + (U64)i * 8 < e && i < MAX_LISTED; i++) {
            U64 site = Q(b + (U64)i * 8);
            if (!site) continue;
            h = (h ^ *(unsigned*)site) * 16777619u;
            U64 slot = Q(site + SLOTS), last = Q(site + SLOTS + 8);
            for (; slot && slot < last; slot += SLOT_BYTES) {
                if (*(int*)(slot + SLOT_KIND) != WASTE_KIND) continue;
                for (U64 it = Q(slot), to = Q(slot + 8); it && it < to; it += SLOT_ENTRY_BYTES) {
                    unsigned bits = *(unsigned*)(it + 8);
                    h = (h ^ bits) * 16777619u;
                    sum += *(float*)(it + 8);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        h = 0;
    }
    *hash = h;
    *amount = sum;
}

// The rest of what the searches read of those buildings: a slot's mode and size, its
// resources and second list, and the offices and vehicles 19f5f0 walks from the building.
static unsigned HashSites(U64 office)
{
    unsigned h = 2166136261u;
    __try {
        U64 b = office ? Q(office + OFFICE_SITES) : 0, e = office ? Q(office + OFFICE_SITES + 8) : 0;
        for (unsigned i = 0; b && b + (U64)i * 8 < e && i < MAX_LISTED; i++) {
            U64 site = Q(b + (U64)i * 8);
            if (!site) continue;
            Mix(h, *(unsigned*)site);
            U64 slot = Q(site + SLOTS), last = Q(site + SLOTS + 8);
            for (; slot && slot < last; slot += SLOT_BYTES) {
                if (*(int*)(slot + SLOT_KIND) != WASTE_KIND) continue;
                Mix(h, *(unsigned*)(slot + SLOT_MODE));
                Mix(h, *(unsigned*)(slot + SLOT_SIZE));
                for (U64 it = Q(slot), to = Q(slot + 8); it && it < to; it += SLOT_ENTRY_BYTES) {
                    U64 resource = Q(it);
                    Mix(h, (unsigned)ResourceIndex(resource));
                    if (resource) Mix(h, *(unsigned*)(resource + RESOURCE_WEIGHT));
                }
                for (U64 it = Q(slot + SLOT_LIMITS), to = Q(slot + SLOT_LIMITS + 8); it && it < to; it += SLOT_ENTRY_BYTES) {
                    Mix(h, (unsigned)ResourceIndex(Q(it)));
                    Mix(h, *(unsigned*)(it + 8));
                    Mix(h, *(unsigned*)(it + 12));
                }
            }
            for (U64 o = Q(site + SITE_OFFICES), oe = Q(site + SITE_OFFICES + 8); o && o < oe; o += 8) {
                U64 linked = Q(o);
                Mix(h, (unsigned)BuildingId(linked));
                if (!linked) continue;
                for (U64 v = Q(linked + OFFICE_VEHICLES), ve = Q(linked + OFFICE_VEHICLES + 8); v && v < ve; v += 8)
                    if (U64 vehicle = Q(v)) Mix(h, *(unsigned*)(vehicle + VEHICLE_ID));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        h = 0;
    }
    return h;
}

// 19e330's two lists: the resources an office's truck may take, and those it skips.
static void HashResourceList(unsigned& h, U64 list)
{
    __try {
        if (!list) return;
        U64 p = Q(list), e = Q(list + 8);
        for (unsigned i = 0; p && p < e && i < MAX_LISTED; p += 8, i++) Mix(h, (unsigned)ResourceIndex(Q(p)));
        Mix(h, 0xFFFFFFFFu); // the end of a list
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Mix(h, 0xEEEEEEEEu);
    }
}

// false: not a waste truck, or it could not be read.
static bool ReadTruck(U64 vehicle, TruckJob* job)
{
    __try {
        U64 type = vehicle ? Q(vehicle + VEHICLE_TYPE) : 0;
        if (!type || *(int*)(type + TYPE_CLASS) != WASTE_KIND) return false;
        job->id = *(int*)(vehicle + VEHICLE_ID);
        job->home = BuildingId(Q(vehicle + VEHICLE_HOME));
        job->in = BuildingId(Q(vehicle + VEHICLE_IN));
        job->state = *(int*)(vehicle + VEHICLE_STATE);
        job->at = *(int*)(vehicle + VEHICLE_PATH_AT);
        job->path = (int)((Q(vehicle + VEHICLE_PATH + 8) - Q(vehicle + VEHICLE_PATH)) / 8);
        U64 stop = Q(vehicle + VEHICLE_STOPS), stopEnd = Q(vehicle + VEHICLE_STOPS + 8);
        job->stops = (int)((stopEnd - stop) / 8);
        job->firstStop = job->stops > 0 ? BuildingId(Q(stop)) : -1;
        unsigned h = 2166136261u;
        for (unsigned i = 0; stop && stop < stopEnd && i < MAX_LISTED; stop += 8, i++) Mix(h, (unsigned)BuildingId(Q(stop)));
        job->stopsHash = h;
        U64 record = Q(vehicle + VEHICLE_RECORDS), recordEnd = Q(vehicle + VEHICLE_RECORDS + 8);
        job->records = (int)((recordEnd - record) / RECORD_BYTES);
        job->firstResource = -1;
        h = 2166136261u;
        for (unsigned i = 0; record && record < recordEnd && i < MAX_LISTED; record += RECORD_BYTES, i++) {
            U64 it = Q(record + RECORD_RESOURCES), to = Q(record + RECORD_RESOURCES + 8);
            if (i == 0 && it && it < to) job->firstResource = ResourceIndex(Q(it));
            for (; it && it < to; it += SLOT_ENTRY_BYTES) {
                Mix(h, (unsigned)ResourceIndex(Q(it)));
                Mix(h, *(unsigned*)(it + 8));
                Mix(h, *(unsigned*)(it + 12));
            }
            Mix(h, 0xFFFFFFFFu);
        }
        job->recordsHash = h;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool VehicleList(U64* begin, U64* end)
{
    __try {
        U64 world = (U64)(g_exeBase + WORLD_RVA);
        *begin = Q(world + VEHICLES_OFFSET);
        *end = Q(world + VEHICLES_OFFSET + 8);
        return *begin && *begin <= *end;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static U64 VehicleAt(U64 slot)
{
    __try {
        return Q(slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void NoteChange(unsigned long long tick, const TruckJob& job, int gone)
{
    TruckChange& c = g_changes[g_changeCount++ % CHANGE_NOTES];
    c.tick = tick;
    c.job = job;
    c.gone = gone;
}

// Every waste truck now: a hash for the search's line, and which changed since the last search.
static unsigned SnapshotTrucks(unsigned long long tick)
{
    unsigned h = 2166136261u;
    U64 begin, end;
    if (!VehicleList(&begin, &end)) return 0;
    EnterCriticalSection(&g_lock);
    unsigned snapshot = ++g_snapshot;
    for (U64 at = begin; at < end; at += 8) {
        TruckJob job = {};
        if (!ReadTruck(VehicleAt(at), &job)) continue;
        for (const unsigned* w = (const unsigned*)&job; w < (const unsigned*)(&job + 1); w++) Mix(h, *w);
        auto found = g_known.find(job.id);
        if (found == g_known.end()) {
            g_known[job.id] = { job, snapshot };
            NoteChange(tick, job, 0);
        } else {
            if (memcmp(&found->second.job, &job, sizeof(job)) != 0) NoteChange(tick, job, 0);
            found->second.job = job;
            found->second.seen = snapshot;
        }
    }
    for (auto it = g_known.begin(); it != g_known.end();) {
        if (it->second.seen != snapshot) {
            NoteChange(tick, it->second.job, 1);
            it = g_known.erase(it);
        } else {
            ++it;
        }
    }
    g_knownTick = tick;
    LeaveCriticalSection(&g_lock);
    return h;
}

// A load starts the ticks again: what was noted before it says nothing about this world.
static void BeginNote(unsigned long long tick)
{
    if (tick < g_lastTick) {
        g_count = g_askedCount = g_changeCount = 0;
        EnterCriticalSection(&g_lock);
        g_known.clear();
        LeaveCriticalSection(&g_lock);
    }
    g_lastTick = tick;
}

static void Keep(const Note& n)
{
    g_notes[g_count++ % NOTES] = n;
}

static bool Tracing()
{
    return Determinism_IsActive() && Determinism_GetMoneyTrace();
}

U64 WasteTrace_FindWaste(U64 world, U64 office, float capacity, U64 list, unsigned char flag, U64* resource, U64 list2)
{
    Note n = {};
    bool trace = Tracing();
    if (trace) {
        n.tick = Determinism_StepCount();
        BeginNote(n.tick);
        HashWaste(office, &n.wasteHash, &n.amount);
        n.siteHash = HashSites(office);
        n.listHash = 2166136261u;
        HashResourceList(n.listHash, list);
        HashResourceList(n.listHash, list2);
        n.truckHash = SnapshotTrucks(n.tick);
        t_inSearch = true;
    }
    U64 r = ((FindWasteFn)(g_exeBase + FIND_WASTE_RVA))(world, office, capacity, list, flag, resource, list2);
    if (trace) {
        t_inSearch = false;
        n.kind = 0;
        n.subject = BuildingId(office);
        n.answer = BuildingId(r);
        n.resource = resource ? ResourceIndex(*resource) : -1;
        n.arg = capacity;
        n.flag = flag;
        Keep(n);
    }
    return r;
}

U64 WasteTrace_FindUnload(U64 world, U64 vehicle, U64 resource, float radius, unsigned char flag)
{
    Note n = {};
    bool trace = Tracing();
    if (trace) {
        n.tick = Determinism_StepCount();
        BeginNote(n.tick);
        __try {
            n.subject = *(int*)(vehicle + VEHICLE_ID);
            n.x = *(float*)(vehicle + VEHICLE_POSITION);
            n.z = *(float*)(vehicle + VEHICLE_POSITION + 8);
            U64 office = Q(vehicle + VEHICLE_HOME);
            HashWaste(office, &n.wasteHash, &n.amount);
            n.siteHash = HashSites(office);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        n.truckHash = SnapshotTrucks(n.tick);
        t_inSearch = true;
    }
    U64 r = ((FindUnloadFn)(g_exeBase + FIND_UNLOAD_RVA))(world, vehicle, resource, radius, flag);
    if (trace) {
        t_inSearch = false;
        n.kind = 1;
        n.answer = BuildingId(r);
        n.resource = ResourceIndex(resource);
        n.arg = radius;
        n.flag = flag;
        Keep(n);
    }
    return r;
}

// 19f5f0's own walk, to name the truck that made its answer yes.
static int TruckOnJob(U64 building, U64 resource)
{
    __try {
        for (U64 o = Q(building + SITE_OFFICES), oe = Q(building + SITE_OFFICES + 8); o && o < oe; o += 8) {
            U64 office = Q(o);
            for (U64 v = Q(office + OFFICE_VEHICLES), ve = Q(office + OFFICE_VEHICLES + 8); v && v < ve; v += 8) {
                U64 vehicle = Q(v);
                U64 type = Q(vehicle + VEHICLE_TYPE);
                if (!type || *(int*)(type + TYPE_CLASS) != WASTE_KIND || *(int*)(vehicle + VEHICLE_STATE) != 0) continue;
                U64 stop = Q(vehicle + VEHICLE_STOPS);
                if (stop == Q(vehicle + VEHICLE_STOPS + 8) || Q(stop) != building) continue;
                if (!resource) return *(int*)(vehicle + VEHICLE_ID);
                U64 record = Q(vehicle + VEHICLE_RECORDS);
                for (U64 it = Q(record + RECORD_RESOURCES), to = Q(record + RECORD_RESOURCES + 8); it < to; it += SLOT_ENTRY_BYTES)
                    if (Q(it) == resource) return *(int*)(vehicle + VEHICLE_ID);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
    return -1;
}

void WasteTrace_Asked(unsigned __int64 building, unsigned __int64 resource, bool taken, bool refused)
{
    if (!t_inSearch) return;
    Asked& a = g_asked[g_askedCount++ % ASKED_NOTES];
    a.tick = Determinism_StepCount();
    a.building = BuildingId(building);
    a.resource = ResourceIndex(resource);
    a.taken = refused ? 2 : taken ? 1 : 0;
    a.truck = taken && !refused ? TruckOnJob(building, resource) : -1;
}

static const char* TruckLine(const TruckJob& j, char* out, size_t size)
{
    _snprintf_s(out, size, _TRUNCATE, "truck %d home %d in %d state %d at %d of %d, %d stop(s) first %d hash %08X, %d record(s) first resource %d hash %08X",
                j.id, j.home, j.in, j.state, j.at, j.path, j.stops, j.firstStop, j.stopsHash, j.records, j.firstResource, j.recordsHash);
    return out;
}

void WasteTrace_Dump(unsigned long long tick)
{
    if (!g_count) {
        Logf("republicmp  WASTE_TRACE: no search yet");
        return;
    }
    unsigned long long first = g_count > NOTES ? g_count - NOTES : 0;
    int written = 0;
    for (unsigned long long i = first; i < g_count; i++) {
        const Note& n = g_notes[i % NOTES];
        if (n.tick > tick || n.tick + NOTES_DUMPED <= tick) continue;
        ++written;
        if (n.kind == 0)
            Logf("republicmp  WASTE_TRACE: tick %llu office %d looks for waste (capacity %.9g, flag %d): building %d, resource %d; the office's waste %.9g hash %08X, sites %08X, lists %08X, trucks %08X",
                 n.tick, n.subject, n.arg, n.flag, n.answer, n.resource, n.amount, n.wasteHash, n.siteHash, n.listHash, n.truckHash);
        else
            Logf("republicmp  WASTE_TRACE: tick %llu truck %d at (%.9g,%.9g) looks to unload resource %d (radius %.9g, flag %d): building %d; its office's waste %.9g hash %08X, sites %08X, trucks %08X",
                 n.tick, n.subject, n.x, n.z, n.resource, n.arg, n.flag, n.answer, n.amount, n.wasteHash, n.siteHash, n.truckHash);
    }
    Logf("republicmp  WASTE_TRACE: %d search(es) in the ticks before %llu, %llu since the load", written, tick, g_count);

    first = g_askedCount > ASKED_NOTES ? g_askedCount - ASKED_NOTES : 0;
    for (unsigned long long i = first; i < g_askedCount; i++) {
        const Asked& a = g_asked[i % ASKED_NOTES];
        if (a.tick > tick || a.tick + NOTES_DUMPED <= tick) continue;
        if (a.taken == 1) Logf("republicmp  WASTE_TRACE: tick %llu building %d resource %d: job taken by truck %d", a.tick, a.building, a.resource, a.truck);
        else Logf("republicmp  WASTE_TRACE: tick %llu building %d resource %d: %s", a.tick, a.building, a.resource, a.taken == 2 ? "refused by the office guard" : "job free");
    }

    char line[192];
    first = g_changeCount > CHANGE_NOTES ? g_changeCount - CHANGE_NOTES : 0;
    for (unsigned long long i = first; i < g_changeCount; i++) {
        const TruckChange& c = g_changes[i % CHANGE_NOTES];
        if (c.tick > tick || c.tick + NOTES_DUMPED <= tick) continue;
        Logf("republicmp  WASTE_TRACE: tick %llu %s %s", c.tick, c.gone ? "gone" : "changed", TruckLine(c.job, line, sizeof(line)));
    }

    // As of the last search, not of this moment: the PCs write this at different ticks.
    std::vector<TruckJob> jobs;
    unsigned long long at;
    EnterCriticalSection(&g_lock);
    at = g_knownTick;
    jobs.reserve(g_known.size());
    for (const auto& k : g_known) jobs.push_back(k.second.job);
    LeaveCriticalSection(&g_lock);
    Logf("republicmp  WASTE_TRACE: the %d waste truck(s) at the search of tick %llu", (int)jobs.size(), at);
    for (const TruckJob& j : jobs) Logf("republicmp  WASTE_TRACE:   %s", TruckLine(j, line, sizeof(line)));
}
