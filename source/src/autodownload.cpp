// autodownload.cpp
// fetching missing media files from http servers (~ akimbo)

#include "cube.h"
#define DEBUGCOND (autodownloaddebug == 1)

struct pckserver
{
    string host;
    int priority, ping, resolved;
    enet_uint32 ip; // address found by the startup ping, network order (0: none)
    hashtable<const char *, int> updates;
    pckserver() { memset(&host, 0, (char *)&updates - (char *)&host); }
};

struct package
{
    string name, fullpath, requestname, host;
    int type, iszip, strictnames, server;
    const char **exts;
    package() { memset(&name, 0, sizeof(struct package)); }
};

vector<pckserver *> pckservers;
vector<package *> pendingpackages;

VARP(autodownload, 0, 1, 1);
VAR(autodownloaddebug, 0, 0, 1);

sl_semaphore sem_pckservers(1, NULL); // control access to the pckservers vector

void resetpckservers()
{
    delfile(findfile("config" PATHDIVS "pcksources.cfg", "w"));
    conoutf("\f3Restart AssaultCube to take effect after resetting the list of packages source servers");
}
COMMAND(resetpckservers, "");

void addpckserver(char *host, char *priority) // add/adjust a package server
{
    sem_pckservers.wait();
    if(*host)
    {
        pckserver *s = NULL;
        loopv(pckservers) if(!strcmp(host, pckservers[i]->host)) s = pckservers[i];
        DEBUG((s ? "updated" : "new") << " host " << host << ", priority " << priority);
        if(!s) s = pckservers.add(new pckserver);
        copystring(s->host, host);
        if(*priority) s->priority = atoi(priority);
    }
    sem_pckservers.post();
}
COMMAND(addpckserver, "ss");

void getpckserver() // return a table of all package servers with four columns
{
    vector<char> res;
    if(sem_pckservers.timedwait(1000)) cvecprintf(res, "\"serverlist is busy, try again later\" "); // single-word return value signals an error
    else
    {
        loopv(pckservers) cvecprintf(res, "\"%s\" %d %d %d\n", pckservers[i]->host, pckservers[i]->priority, pckservers[i]->ping, pckservers[i]->resolved);
        sem_pckservers.post();
    }
    resultcharvector(res, -1);
}
COMMAND(getpckserver, "");

SDL_mutex *pckpinglog_lock; // auxilliary locking for pckping_log
vector<char> pckping_log; // access controlled by sem_pckservers (and pckpinglog_lock)
const char *updatestxt = "/updates.txt";

#ifdef __3DS__
static SDL_atomic_t stop_package_discovery;

static int package_discovery_cancel(void *, float)
{
    return SDL_AtomicGet(&stop_package_discovery);
}
#endif

int pingpckserver(void *data) // fetch updates.txt from a media server, measure required time
{
    httpget h;
#ifdef __3DS__
    if(SDL_AtomicGet(&stop_package_discovery)) return 0;
    h.callbackfunc = package_discovery_cancel;
#endif
    urlparse u;
    pckserver *s = (pckserver *)data;
    u.set(s->host);
    s->resolved = h.set_host(u.domain) ? 1 : 0;
    s->ip = s->resolved ? h.ip.host : 0;
    SDL_mutexP(pckpinglog_lock);
    string b;
    cvecprintf(pckping_log, "lresolving hostname %s %s (%s)\n", u.domain, s->resolved ? "succeeded" : "failed", iptoa(ENET_NET_TO_HOST_32(h.ip.host), b));
    SDL_mutexV(pckpinglog_lock);
    if(s->resolved)
    {
        if(*u.port) h.set_port(atoi(u.port));
        defformatstring(url)("%s%s", u.path, updatestxt);
        int res = h.get(url, 5000, 10000, 0, true); // http HEAD request only - to get a nice ping value
        if(res >= 0 && h.response > 0)
        {
            s->ping = h.elapsedtime;
            SDL_mutexP(pckpinglog_lock);
            cvecprintf(pckping_log, "lhttp head %s:%d%s response time: %d, status: %d, contentlength: %d\n", u.domain, h.ip.port, url, s->ping, h.response, h.contentlength);
#ifdef _DEBUG
            if(DEBUGCOND) { cvecprintf(pckping_log, "l%s", escapestring(h.header, true, true)); pckping_log.add('\n'); }
#endif
            SDL_mutexV(pckpinglog_lock);
            if(h.response == 200
#ifdef __3DS__
                && !SDL_AtomicGet(&stop_package_discovery)
#endif
            )
            {
                h.outvec = new vector<uchar>;
                res = h.get(url, 5000, 20000);     // http GET
                if(res >= 0 && h.response == 200)
                { // parse updates.txt
                    h.outvec->add('\0');
                    s->updates.clear();
                    parseupdatelist(s->updates, (char *)h.outvec->getbuf()); // don't filter entries
                    SDL_mutexP(pckpinglog_lock);
                    cvecprintf(pckping_log, "lgot %s:%d%s: %d bytes, %d valid update lines\n", u.domain, h.ip.port, url, res, s->updates.numelems);
                    SDL_mutexV(pckpinglog_lock);
                }
                else
                {
                    SDL_mutexP(pckpinglog_lock);
                    cvecprintf(pckping_log, "lretrieving %s:%d%s failed: response %d, err: %s\n", u.domain, h.ip.port, url, h.response, h.err ? h.err : "(null)");
#ifdef _DEBUG
                    if(DEBUGCOND)
                    {
                        if(!h.header) { h.rawrec.setsize(min(h.rawrec.length(), MAXSTRLEN)); h.rawrec.add('\0'); }
                        cvecprintf(pckping_log, "lheader: %s", escapestring(h.header ? h.header : h.rawrec.getbuf(), true, true)); pckping_log.add('\n');
                    }
#endif
                    SDL_mutexV(pckpinglog_lock);
                }
                DELETEP(h.outvec);
            }
        }
        else
        { // connection to server failed
            SDL_mutexP(pckpinglog_lock);
            cvecprintf(pckping_log, "lpinging %s failed, err: %s\n", s->host, h.err ? h.err : "(null)");
            SDL_mutexV(pckpinglog_lock);
        }
    }
    h.disconnect();
    return 0;
}

int pingallpckservers(void *data)
{
    sem_pckservers.wait();
    if(autodownload)
    {
        pckpinglog_lock = SDL_CreateMutex();
        vector<SDL_Thread *> pckthreads;
        int nop, good = 0, disabled = 0;
        loopv(pckservers) if(pckservers[i]->priority > -1000) pckthreads.add(SDL_CreateThread(pingpckserver, "PingPackageServer", pckservers[i])); // start pinging all servers at once
        loopv(pckthreads) if(pckthreads[i]) SDL_WaitThread(pckthreads[i], &nop); // wait for all ping threads to finish
        SDL_DestroyMutex(pckpinglog_lock);
        loopv(pckservers)
        {
            pckserver *s = pckservers[i];
            if(s->priority <= -1000) disabled++;
            else if(s->resolved && s->ping) good++;
        }
        int bad = pckservers.length() - good - disabled;
        cvecprintf(pckping_log, "csuccessfully pinged %d media server%s, %d failure%s, %d disabled\n", good, good == 1 ? "" : "s", bad, bad == 1 ? "" : "s", disabled);
    }
    else cvecprintf(pckping_log, "c\f4(automatic media file download deactivated)\n");
    pckping_log.add('e'); // signal end of thread
    sem_pckservers.post();
    return 0;
}

SDL_Thread *pingallpckthread = NULL;

void setupautodownload()
{
    // fetch updates from all configured servers
    // in a background thread
    // during startup
    pingallpckthread = SDL_CreateThread(pingallpckservers, "PingAllPackageServers", NULL);
}

#ifdef __3DS__
void ac3ds_stop_package_discovery()
{
    SDL_AtomicSet(&stop_package_discovery, 1);
    if(pingallpckthread)
    {
        SDL_WaitThread(pingallpckthread, NULL);
        pingallpckthread = NULL;
    }
}
#endif

void pollautodownloadresponse() // thread-safe feedback from the autodownload-updater thread
{
    static int donepolling = 0;
    if(donepolling) return;
    if(!sem_pckservers.trywait())
    {
        if(pckping_log.length())
        {
            pckping_log.add('\0');
            char *p, *l = pckping_log.getbuf();
            do
            { // break into single lines
                if((p = strchr(l, '\n'))) *p = '\0';
                if(*l == 'c') conoutf("%s", l + 1);  // line starts with "c" -> console
                else if(*l == 'e') donepolling = 1;  // line starts with "e" -> end polling
                else clientlogf("%s", l + 1);        // line starts with "l" -> log
                l = p + 1;
            }
            while(p);
            pckping_log.setsize(0);
        }
        sem_pckservers.post();
    }
    if(donepolling)
    {
        int nop;
        SDL_WaitThread(pingallpckthread, &nop);    // detaching the thread would be preferrable, but SDL 1.2 doesn't provide that
        pingallpckthread = NULL;
    }
}

const char *mmexts[] = { ".md2", ".md3", ".cfg", ".txt", ".jpg", ".png", "" },
           *mapexts[] = { ".cgz", ".cfg", "" },
           *sbexts[] = { "_lf.jpg", "_rt.jpg", "_ft.jpg", "_bk.jpg", "_dn.jpg", "_up.jpg", "_license.txt", "" };

bool requirepackage(int type, const char *name, const char *host)
{
    const char *prefix = NULL, *checkprefix = NULL, *forceext = NULL;
    package *pck = new package;
    switch(type)
    {
        case PCK_AUDIO:
            prefix = "packages/audio/";
            checkprefix = "ambience/";
            forceext = ".ogg";
            break;

        case PCK_MAPMODEL:
            prefix = "packages/models/";
            checkprefix = "mapmodels/";
            pck->iszip = 1;
            pck->exts = mmexts;
            break;

        case PCK_TEXTURE:
            checkprefix = "packages/";
            forceext = ".jpg";
            break;

        case PCK_SKYBOX:
            prefix = "packages/textures/skymaps/";
            pck->iszip = pck->strictnames = 1;
            pck->exts = sbexts;
            break;

        case PCK_MAP:
            prefix = "packages/maps/";
            pck->iszip = pck->strictnames = 1;
            pck->exts = mapexts;
            break;

        case PCK_MOD:
            prefix = "mods/";
            forceext = ".zip";
            break;

        default:
            conoutf("\f3requirepackage: illegal package type %d (\"%s\")", type, name); // should probably be fatal()
            delete pck;
            return false;
    }
    string upath;
    copystring(upath, name);
    unixpath(upath);
    filtertext(pck->name, name, FTXT__MEDIAFILEPATH); // also changes backslashes to slashes
    if(strlen(upath) > 100 || strcmp(upath, pck->name) || (checkprefix && strncmp(pck->name, checkprefix, strlen(checkprefix))) || (type == PCK_MOD && !validzipmodname(upath)))
    {
        conoutf("\f3requirepackage: illegal media path \"%s\" (type: %d)", name, type);
        delete pck;
        return false;
    }
    if(forceext && strlen(pck->name) > strlen(forceext) && !strcmp(pck->name + strlen(pck->name) - strlen(forceext), forceext)) forceext = NULL; // filename already has required extension
    formatstring(pck->fullpath)("%s%s%s", prefix ? prefix : "", pck->name, forceext ? forceext : "");
    formatstring(pck->requestname)("/%s%s%s", pck->fullpath, type == PCK_MAP ? ".cgz" : "", pck->iszip ? ".zip" : ""); // special case map zips: file ending .cgz.zip

    loopv(pendingpackages) if(!strcmp(pendingpackages[i]->requestname, pck->requestname))
    { // request already queued
        delete pck;
        return false;
    }
    pck->type = type;
    if(host) copystring(pck->host, host); // assign fixed server
    DEBUG("name: " << pck->name << ", fullpath: " << pck->fullpath << ", requestname: " << pck->requestname << ", host: " << pck->host << ", type: " << pck->type << ", iszip: " << pck->iszip
          << ", strictnames: " << pck->strictnames << ", server: " << pck->server << ", exts: " << (pck->exts ? *pck->exts : ""));
    pendingpackages.add(pck);
    return true;
}

VAR(rereadtexturelists, 0, 1, 1); // flag to indicate additional texture files to trigger a menu rebuild (defaults to "1" to build the menus after startup)
VAR(rereadsoundlists, 0, 1, 1); // same for map sounds

static int installedfiles = 0; // files written by the last processdownload()
#ifdef __3DS__
static bool downloadcheckpoint();
#endif

bool processdownload(package *pck, stream *f) // write downloaded data to file(s)
{
    installedfiles = 0;
    if(!f || !f->seek(0)) { delete f; return false; }
#ifdef __3DS__
    if(downloadcheckpoint()) { delete f; return false; }
#endif
    bool installed = false;
    if(pck->iszip)
    { // unpack zip file
        vector<const char *> files;
        void *mz = zipmanualopen(f, files);
        if(mz)
        {
            string filename, cleanzname;
            bool complete = true;
            loopv(files)
            {
                if(i >= MAXFILESINADZIP) break;
#ifdef __3DS__
                if(downloadcheckpoint()) { complete = false; break; }
#endif
                const char *zname = behindpath(files[i]); // ignore _all_ paths inside the zip
                filtertext(cleanzname, zname, FTXT__MEDIAFILENAME);
                bool isok = false;
                for(int n = 0; *pck->exts[n]; n++)
                {
                    if(pck->strictnames)
                    { // only base names and listed extensions allowed
                        formatstring(filename)("%s%s", pck->fullpath, pck->exts[n]);
                        isok = !strcmp(behindpath(filename), zname);
                    }
                    else
                    { // any name with listed extension, written to subdirectory
                        int zlen = (int)strlen(zname), elen = (int)strlen(pck->exts[n]);
                        if((isok = (zlen > elen && !strcmp(zname + zlen - elen, pck->exts[n])))) formatstring(filename)("%s/%s", pck->fullpath, zname);
                    }
                    if(isok) break;
                }
                if(isok && !strcmp(cleanzname, zname))
                {
#ifdef __3DS__
                    stream *entry = zipmanualstream(mz, i);
                    bool saved = entry && installfile3ds(path(filename), entry, MAXMEDIADOWNLOADFILESIZE, true, downloadcheckpoint);
                    delete entry;
                    if(saved) { installed = true; installedfiles++; }
                    else complete = false;
#else
                    stream *zf = openfile(path(filename), "wb");
                    if(zf)
                    {
                        int got = zipmanualread(mz, i, zf, MAXMEDIADOWNLOADFILESIZE); // extract file from zip and write to @filename
                        DEBUGCODE(clientlogf("extracted %d bytes from %s %s, written to %s", got, pck->name, files[i], filename));
                        if(got >= 0) installed = true;
                        else complete = false;
                        delete zf;
                    }
                    else complete = false;
#endif
                }
                else conoutf("\f3illegal filename \"%s\" in %s", files[i], pck->requestname);
            }
            zipmanualclose(mz);
            if(!complete) installed = false;
        }
        else clientlogf("failed to open zip file %s", pck->requestname);
    }
    else
    { // write single file
        int maxsize = pck->type == PCK_MOD ? MAXMODDOWNLOADSIZE : MAXMEDIADOWNLOADFILESIZE;
#ifdef __3DS__
        installed = installfile3ds(path(pck->fullpath, true), f, maxsize, true, downloadcheckpoint);
        if(installed) installedfiles++;
#else
        stream *d = openfile(path(pck->fullpath, true), "wb");
        if(d)
        {
            long size = f->size();
            installed = size >= 0 && size <= maxsize && streamcopy(d, f, maxsize) == size;
            delete d;
        }
#endif
        delete f;
    }

    if(!installed) return false;
    switch(pck->type) // some housekeeping
    {
        case PCK_SKYBOX:
        case PCK_TEXTURE:
            rereadtexturelists = 1;
            break;

        case PCK_AUDIO:
            rereadsoundlists = 1;
            break;

        case PCK_MAP:
        case PCK_MAPMODEL:
            break;
    }
    return true;
}

bool canceldownloads = false;
int progress_n, progress_of;

#ifdef __3DS__
// Seconds into a map load after which missing content is no longer waited
// for while connected to a server (0: no limit); the map is then played
// without it, and the next load of that map fetches what is still missing.
// On the handheld the downloads of one custom map once held a map change for
// 96 seconds with nothing serviced, and the server dropped the connection.
// Keeping the connection serviced is not enough on its own: a stock server
// also kicks as AFK, about 48 seconds after a map change, a player who has
// not spawned yet (five or more players in a team mode, or a full server),
// so 0 and values above about 40 get the console kicked there.
VARP(ac3dsdownloadwait, 0, 30, 180);
static uint downloadstart, downloadservice;
static int downloadlimit = -1;     // milliseconds, -1: none
static bool downloadexpired = false;

static bool downloadcheckpoint()
{
    if(canceldownloads) return true;
    const uint now = SDL_GetTicks();
    if(downloadlimit >= 0 && now - downloadstart >= uint(downloadlimit))
    {
        downloadexpired = canceldownloads = true;
        return true;
    }
    // The time check runs before every write. Poll HID and service ENet at
    // most ten times a second, regardless of how many blocks are staged.
    if(now - downloadservice >= 100)
    {
        downloadservice = now;
        c2skeepalive();
        if(interceptkey(SDLK_ESCAPE)) canceldownloads = true;
    }
    return canceldownloads;
}

static void finishcancelledpackages()
{
    if(downloadexpired) conoutf("\f3missing content not fetched within %d seconds of the map change: playing without it (ac3dsdownloadwait sets the limit)", ac3dsdownloadwait);
    if(canceldownloads) pendingpackages.deletecontents();
}
#define CANCELKEYNAME "START"
#else
#define CANCELKEYNAME "ESC"
#endif

int progress_callback_dlpackage(void *data, float progress)
{
    // The HTTP client calls this at least four times a second while it waits
    // or transfers. Upstream only services the server connection between the
    // stages of a map load, which a PC download finishes well within.
#ifdef __3DS__
    if(downloadcheckpoint()) return 1;
    // A transfer calls in for every block it receives.
    static uint lastdrawn = 0;
    const uint now = SDL_GetTicks();
    if(now - lastdrawn < 100 && progress < 1.0f) return 0;
    lastdrawn = now;
#else
    c2skeepalive();
#endif
    if(progress_of < 0)
    {
        defformatstring(txt)("downloading %s... (" CANCELKEYNAME " to abort)", (const char *)data);
        if(progress < 0) show_out_of_renderloop_progress(progress + 1.0f, "waiting for response (" CANCELKEYNAME " to abort)");
        else show_out_of_renderloop_progress(min(progress, 1.0f), txt);
    }
    else loadingscreen("downloading package %d of %d...\n%s  \fs%s%d%%\fr\n(" CANCELKEYNAME " to cancel)", progress_n, progress_of, (const char *)data, progress < 0 ? "\f3waiting " : "", int(100.0 * fabs(progress)));
#ifndef __3DS__
    if(interceptkey(SDLK_ESCAPE))
    {
        canceldownloads = true;
        return 1;
    }
#endif
    return 0;
}

bool dlpackage(httpget &h, package *pck, pckserver *s) // download one package from one server
{
    urlparse u;
    u.set(s ? s->host : pck->host);
    h.callbackfunc = progress_callback_dlpackage;
    h.callbackdata = pck->name;
    progress_n = progress_of - pendingpackages.length() + 1;
#ifdef __3DS__
    if(downloadcheckpoint()) return false;
    h.service_dns = true;
    stopwatch phase;
    phase.start();
#endif
    // A listed server was already resolved by the startup ping.
    if(*u.domain && h.set_host(u.domain, s ? s->ip : 0))
    {
#ifdef __3DS__
        const int resolved = phase.elapsed();
        if(downloadcheckpoint()) return false;
#endif
        h.set_port(*u.port ? atoi(u.port) : 80);
        h.outstream = openvecfile(NULL);
        defformatstring(url)("%s%s", u.path, pck->requestname);
        int got = h.get(url, 6000, 90000);
        if(got < 0 || !h.response)
        {
            if(!canceldownloads && s) s->ping = 0; // received a hard error from the server connection, better not try this one again
            clientlogf("download %s:%d%s failed, err: %s", u.domain, h.ip.port, url, h.err ? h.err : "(null)");
            h.disconnect();
        }
        else
        {
            if(h.response == 200)
            {
                DEBUGCODE(clientlogf("downloaded %s:%d%s, %d bytes (%d raw), %d msec", u.domain, h.ip.port, url, got, h.contentlength, h.elapsedtime));
#ifdef __3DS__
                // Where the time goes on the handheld: name lookup, transfer, writing to the card.
                phase.start();
                if(progress_of >= 0 && !downloadcheckpoint()) loadingscreen("installing package %d of %d...\n%s\n(" CANCELKEYNAME " to cancel)", progress_n, progress_of, pck->name);
#endif
                bool installed = processdownload(pck, h.outstream);
                h.outstream = NULL; // deleted by processdownload()
#ifdef __3DS__
                clientlogf("package %s: host lookup %d ms, %d bytes in %d ms, %d files written in %d ms", pck->requestname, resolved, got, int(h.elapsedtime), installedfiles, int(phase.elapsed()));
#endif
                if(!installed && !canceldownloads) clientlogf("could not install downloaded package %s", pck->requestname);
                return installed;
            }
            else clientlogf("download %s:%d%s failed, server response %d%s%s", u.domain, h.ip.port, url, h.response, h.err ? ", err: " : "", h.err ? h.err : "");
        }
        DELETEP(h.outstream);
    }
    else
    {
        if(s && !canceldownloads) s->resolved = 0;
        clientlogf("resolving host \"%s\" failed", u.domain);
        h.disconnect();
    }
    return false;
}

int packagesort_server(package **a, package **b) { return (*b)->server - (*a)->server; }
int packagesort_name(package **a, package **b) { return strcmp((*b)->name, (*a)->name); }
int packagesort_host(package **a, package **b) { return strcmp((*b)->host, (*a)->host); }

int pckserversort(pckserver **a, pckserver **b)
{
    if((*a)->resolved != (*b)->resolved) return (*b)->resolved - (*a)->resolved; // group resolvable hosts at the top
    if((*a)->priority != (*b)->priority) return (*b)->priority - (*a)->priority; // manual priority wins over ping (high priority to the top)
    if((*a)->ping && (*b)->ping) return (*a)->ping - (*b)->ping; // smaller ping wins
    return (*b)->ping - (*a)->ping; // or whichever ping is not zero
}

int downloadpackages(bool loadscr) // get all pending packages
{
    bool failed = false;
    httpget h;
    canceldownloads = false;
    progress_of = loadscr ? pendingpackages.length() : -1;
#ifdef __3DS__
    if(pendingpackages.empty()) return 0;
    downloadexpired = false;
    downloadlimit = -1;
    if(loadscr && multiplayer(NULL) && ac3dsdownloadwait)
    {
        // Counted from the start of the load, not of this stage: loadscr is
        // only set for the call from load_world().
        extern int ac3ds_load_elapsed();
        downloadlimit = max(ac3dsdownloadwait * 1000 - ac3ds_load_elapsed(), 0);
    }
    downloadstart = SDL_GetTicks();
    downloadservice = downloadstart - 100;
    if(downloadcheckpoint()) { finishcancelledpackages(); return 0; }
    while(sem_pckservers.timedwait(50))
    {
        if(downloadcheckpoint()) { finishcancelledpackages(); return 0; }
        if(SDL_GetTicks() - downloadstart < 1000) continue;
        conoutf("\f3failed to download packages: still pinging the servers...");
        return 0;
    }
#else
    if(sem_pckservers.timedwait(1000))
    { // can't lock the server list -> this means, the server ping threads have not finished yet
        if(pendingpackages.length()) conoutf("\f3failed to download packages: still pinging the servers...");
        return 0;
    }
#endif
    pckservers.sort(pckserversort);
    loopv(pendingpackages)
    {
        package *pck = pendingpackages[i];
        int maxrev = 0, *rev = NULL;
        pck->server = -1;                  // server with highest revision for that content
        loopvj(pckservers)
        {
            pckserver *s = pckservers[j];
            if(s->priority > -1000 && s->resolved && s->ping && (rev = s->updates.access(pck->requestname)) && *rev > maxrev)
            {
                maxrev = *rev;
                pck->server = j;
            }
        }
    }

    // get all packages with auto-assigned servers
    pendingpackages.sort(packagesort_server);
    loopvrev(pendingpackages)
    {
        if(canceldownloads) break;
        package *pck = pendingpackages[i];
        if(!*pck->host && pckservers.inrange(pck->server) && pckservers[pck->server]->resolved)
        {
            if(dlpackage(h, pck, pckservers[pck->server])) delete pendingpackages.remove(i);
            else pck->server = -1; // auto-assigned server didn't work - try any other now
        }
    }

    // try all packages from all servers - high priority servers first
    pendingpackages.sort(packagesort_name);
    loopvj(pckservers)
    {
        pckserver *s = pckservers[j];
        if(s->priority <= -1000 || !s->resolved || !s->ping) continue;
        loopv(pendingpackages)
        {
            if(canceldownloads) break;
            package *pck = pendingpackages[i];
            if(!*pck->host)
            {
                if(dlpackage(h, pck, s)) delete pendingpackages.remove(i--);
            }
            if(!s->resolved || !s->ping) break;
        }
    }
#ifndef __3DS__
    if(canceldownloads) pendingpackages.deletecontents();
#endif

    // print error messages for all failed packages - and remove them
    loopv(pendingpackages)
    {
        if(canceldownloads) break;
        package *pck = pendingpackages[i];
        if(!*pck->host)
        {
            conoutf("\f3failed to load %s from any host, giving up", pck->name);
            delete pendingpackages.remove(i--);
            failed = true;
        }
    }

    // get all packages with manually assigned hosts - those will be deleted from the list regardless of success!
    pendingpackages.sort(packagesort_host);
    loopvrev(pendingpackages)
    {
        if(canceldownloads) break;
        package *pck = pendingpackages[i];
        if(!dlpackage(h, pck, NULL) && !canceldownloads)
        {
            conoutf("\f3failed to load %s from %s, giving up", pck->name, pck->host);
            failed = true;
        }
        delete pendingpackages.remove(i);
    }
    sem_pckservers.post();
    h.disconnect();
#ifdef __3DS__
    finishcancelledpackages();
#else
    if(canceldownloads) pendingpackages.deletecontents();
#endif
    return failed ? 0 : h.traffic;
}

void writepcksourcecfg()
{
#ifdef __3DS__
    ac3ds_stop_package_discovery();
#endif
    if(pckservers.length())
    {
        stream *f = openfile("config" PATHDIVS "pcksources.cfg", "w");
        if(!f) return;
        f->printf("// list of package source servers (only add servers you trust!)\n\n");
        f->printf("autodownloaddebug %d\n", autodownloaddebug); // saved.cfg is loaded too late for this to be in it and be efficient...
        loopv(pckservers)
        {
            pckserver *s = pckservers[i];
            if(s->priority > -10000) f->printf("addpckserver %s %d // ping: %d, resolved: %d, updates: %d\n", s->host, s->priority, s->ping, s->resolved, s->updates.numelems);
        }
        delete f;
    }
}

void getmod(char *name)
{
    if(requirepackage(PCK_MOD, name) && downloadpackages(false)) conoutf("mod package %s successfully downloaded", name);
    else conoutf("failed to download mod package %s", name);
}
COMMAND(getmod, "s");
