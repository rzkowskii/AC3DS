#include "cube.h"

int wx1, wy1, wx2, wy2;
float wsx1, wsy1, wsx2, wsy2;

VARP(watersubdiv, 1, 4, 64);
FVARF(waterlevel, -10000, -10000, 127,
{
    if(!noteditmode("waterlevel"))
    {
        hdr.waterlevel = waterlevel * WATERLEVELSCALING;
        unsavededits++;
    }
    waterlevel = float(hdr.waterlevel) / WATERLEVELSCALING; // waterlevel and hdr.waterlevel need to be kept in sync
});

void setwatercolor(const char *r, const char *g, const char *b, const char *a)
{
    if(r[0])
    {
        hdr.watercolor[0] = ATOI(r);
        hdr.watercolor[1] = ATOI(g);
        hdr.watercolor[2] = ATOI(b);
        hdr.watercolor[3] = a[0] ? ATOI(a) : 178;
    }
    else
    {
        hdr.watercolor[0] = 20;
        hdr.watercolor[1] = 25;
        hdr.watercolor[2] = 20;
        hdr.watercolor[3] = 178;
    }
    if(editmode) unsavededits++;
}

COMMANDF(watercolour, "ssss", (char *r, char *g, char *b, char *a) { if(editmode) setwatercolor(r, g, b, a); });

FVAR(waveheight, 0, 0.3f, 1.0f);
VARP(ignoreoverride_limitwaveheight, 0, 0, 1);
float effective_waveheight = 0.3f;

static float waterwaveheight()
{
    extern int mapoverride_limitwaveheight;
    return mapoverride_limitwaveheight && !ignoreoverride_limitwaveheight ? 0.1f : waveheight;
}

void getwatercolour()
{
    defformatstring(res)("%d %d %d %d", hdr.watercolor[0], hdr.watercolor[1], hdr.watercolor[2], hdr.watercolor[3]);
    result(res);
}
COMMAND(getwatercolour, "");

const char *componentnames[] = { "red", "green", "blue", "alpha", "" };

void setwatercolour(char *what, int *v)
{
    int t = getlistindex(what, componentnames, true, -1);
    if(t >= 0 && t < 4 && editmode)
    {
        hdr.watercolor[t] = *v;
        unsavededits++;
    }
}
COMMAND(setwatercolour, "si");

// renders water for bounding rect area that contains water... simple but very inefficient

#define VERTW(vertw, defbody, body) \
    static inline void def##vertw() \
    { \
        varray::defattrib(varray::ATTRIB_VERTEX, 3, GL_FLOAT); \
        defbody; \
    } \
    inline void vertw(float v1, float v2, float v3, float t) \
    { \
        float angle = v1*v2*0.1f/PI2 + t; \
        float s = angle - int(angle) - 0.5f; \
        s *= 8 - fabs(s)*16; \
        float h = effective_waveheight*0.5f*s; \
        varray::attrib<float>(v1, v2, v3+h); \
        body; \
    }
#define VERTWC(vertwc, defbody, body) \
    VERTW(vertwc, defbody, { \
        float v = angle - int(angle+0.25) - 0.25; \
        v *= 8 - fabs(v)*16; \
        body; \
    })
#define VERTWT(vertwt, defbody, body) \
    VERTWC(vertwt, defbody, { \
        float duv = effective_waveheight*0.5f*v; \
        body; \
    })

VERTW(vertw, {}, {})
VERTWC(vertwc, {
    varray::defattrib(varray::ATTRIB_COLOR, 4, GL_UNSIGNED_BYTE);
}, {
    varray::attrib<uchar>(hdr.watercolor[0], hdr.watercolor[1], hdr.watercolor[2], clamp(int(hdr.watercolor[3] + fabs(s)*0x18), 0, 255));
})
VERTWT(vertwt, {
    varray::defattrib(varray::ATTRIB_TEXCOORD0, 3, GL_FLOAT);
}, {
    varray::attrib<float>(v1+duv, v2+duv, v3+h);
})
VERTWT(vertwtc, {
    varray::defattrib(varray::ATTRIB_COLOR, 4, GL_UNSIGNED_BYTE);
    varray::defattrib(varray::ATTRIB_TEXCOORD0, 3, GL_FLOAT);
}, {
    varray::attrib<uchar>(255, 255, 255, int(0x33 + fabs(s)*0x18));
    varray::attrib<float>(v1+duv, v2+duv, v3+h);
})
#ifdef __3DS__
VERTWT(vertwtc_refract, {
    varray::defattrib(varray::ATTRIB_COLOR, 4, GL_UNSIGNED_BYTE);
    varray::defattrib(varray::ATTRIB_TEXCOORD0, 3, GL_FLOAT);
}, {
    varray::attrib<uchar>(255, 255, 255, int(0x33 + fabs(s)*0x18));
    varray::attrib<float>(v1-duv, v2-duv, v3+h);
})
#else
VERTWT(vertwmtc, {
    varray::defattrib(varray::ATTRIB_COLOR, 4, GL_UNSIGNED_BYTE);
    varray::defattrib(varray::ATTRIB_TEXCOORD0, 3, GL_FLOAT);
    varray::defattrib(varray::ATTRIB_TEXCOORD1, 3, GL_FLOAT);
}, {
    varray::attrib<uchar>(255, 255, 255, int(0x33 + fabs(s)*0x18));
    varray::attrib<float>(v1-duv, v2+duv, v3+h);
    varray::attrib<float>(v1+duv, v2+duv, v3+h);
})
#endif

extern int nquads;

#ifdef __3DS__
namespace
{
    struct WaterRun { int x, y0, y1; };
    struct WaterRows { int y0, y1; };
    struct ProjectedWaterVertex { float x, y, z; uchar color[4]; float u, v, w; };
    static_assert(sizeof(ProjectedWaterVertex) == 28, "PICA projected vertex layout");
    vector<WaterRun> waterruns;
    vector<WaterRows> waterrows;
    vector<ProjectedWaterVertex> waterreflectionvertices, waterrefractionvertices;
    vector<int> waterbatchends;
    int projectedwaterquads;

    struct WaterFrustum
    {
        struct Point { double x, y; };
        float x[6], y[6], base[6];

        WaterFrustum(float level, float wave)
        {
            // Clip-space half-spaces avoid dividing by W near the camera.
            // Fold the full vertical wave envelope into each plane.
            loopi(6)
            {
                const int row = i / 2;
                const float sign = i & 1 ? -1.f : 1.f;
                x[i] = mvpmatrix[3] + sign * mvpmatrix[row];
                y[i] = mvpmatrix[7] + sign * mvpmatrix[row+4];
                const float z = mvpmatrix[11] + sign * mvpmatrix[row+8];
                const float w = mvpmatrix[15] + sign * mvpmatrix[row+12];
                // Leave margin for differences in PICA matrix precision.
                const float margin = (fabsf(x[i]) + fabsf(y[i]) + fabsf(z)) * 0.125f + 0.0001f;
                base[i] = z * (level + (z < 0 ? -wave : wave)) + w + margin;
            }
        }

        void column(int left, int right, float (&support)[6]) const
        {
            loopi(6) support[i] = base[i] + x[i] * (x[i] < 0 ? left : right);
        }

        bool contains(const float (&support)[6], int top, int bottom) const
        {
            loopi(6) if(support[i] + y[i] * (y[i] < 0 ? top : bottom) < 0) return false;
            return true;
        }

        int clip(Point (&polygon)[12], int count) const
        {
            // Each plane was maximized independently over the wave's Z
            // range. Their XY intersection therefore contains every point
            // of the displaced surface which can pass all six clip planes.
            for(int plane = 0; plane < 6 && count; ++plane)
            {
                Point clipped[12];
                int length = 0;
                Point previous = polygon[count-1];
                double before = x[plane]*previous.x + y[plane]*previous.y + base[plane];
                loopi(count)
                {
                    const Point next = polygon[i];
                    const double after = x[plane]*next.x + y[plane]*next.y + base[plane];
                    if((before < 0 && after > 0) || (before > 0 && after < 0))
                    {
                        const double t = before / (before - after);
                        clipped[length++] = {previous.x + (next.x-previous.x)*t,
                                             previous.y + (next.y-previous.y)*t};
                    }
                    if(after >= 0) clipped[length++] = next;
                    previous = next;
                    before = after;
                }
                memcpy(polygon, clipped, length * sizeof(Point));
                count = length;
            }
            return count;
        }
    };

    int watergridend(int start, int end)
    {
        return start + ((end - start + watersubdiv - 1) / watersubdiv) * watersubdiv;
    }

    void buildwaterruns(float level)
    {
        const WaterFrustum frustum(level, effective_waveheight * 0.5f);
        const int xend = watergridend(wx1, wx2), yend = watergridend(wy1, wy2);
        const int patch = watersubdiv * 8;
        waterruns.setsize(0);
        for(int x = wx1; x < xend; x += patch)
        {
            const int right = min(x + patch, xend);
            float columnplane[6];
            frustum.column(x, right, columnplane);
            waterrows.setsize(0);
            for(int y = wy1; y < yend; y += patch)
            {
                const int bottom = min(y + patch, yend);
                if(!frustum.contains(columnplane, y, bottom)) continue;
                if(!waterrows.empty() && waterrows.last().y1 == y) waterrows.last().y1 = bottom;
                else waterrows.add({y, bottom});
            }
            for(int column = x; column < right; column += watersubdiv)
                loopv(waterrows) waterruns.add({column, waterrows[i].y0, waterrows[i].y1});
        }
    }

    template<void (*emit)(float, float, float, float)>
    void drawwaterruns(float level, float time)
    {
        // Join runs with degenerate triangles. Each run has an even vertex
        // count, so the original strip winding and blend order are unchanged.
        int count = 0, lastx = 0, lasty = 0;
        loopv(waterruns)
        {
            const WaterRun &run = waterruns[i];
            const int length = (run.y1 - run.y0) / watersubdiv + 1;
            if(count && count + 2 + length * 2 > 8190)
            {
                varray::end();
                count = 0;
            }
            if(!count) varray::begin(GL_TRIANGLE_STRIP);
            else
            {
                emit(lastx, lasty, level, time);
                emit(run.x, run.y0, level, time);
                count += 2;
            }
            for(int y = run.y0; y <= run.y1; y += watersubdiv)
            {
                emit(run.x, y, level, time);
                emit(run.x + watersubdiv, y, level, time);
            }
            count += length * 2;
            nquads += length / 2;
            lastx = run.x + watersubdiv;
            lasty = run.y1;
        }
        if(count) varray::end();
    }

    bool buildprojectedwater(float level, float time, bool refract)
    {
        waterreflectionvertices.setsize(0);
        waterrefractionvertices.setsize(0);
        waterbatchends.setsize(0);
        projectedwaterquads = 0;
        // Dense grids on very large maps keep the bounded, streaming path.
        const int maxvertices = 32768;
        int upperbound = 0;
        loopv(waterruns)
        {
            const WaterRun &run = waterruns[i];
            upperbound += 2 * ((run.y1 - run.y0) / watersubdiv + 2);
            if(upperbound > maxvertices) return false;
        }
        waterreflectionvertices.reserve(upperbound);
        if(refract) waterrefractionvertices.reserve(upperbound);
        auto emit = [=](float x, float y)
        {
            const float angle = x*y*0.1f/PI2 + time;
            float s = angle - int(angle) - 0.5f;
            s *= 8 - fabs(s)*16;
            float v = angle - int(angle+0.25) - 0.25;
            v *= 8 - fabs(v)*16;
            const float z = level + effective_waveheight*0.5f*s;
            const float duv = effective_waveheight*0.5f*v;
            const uchar alpha = int(0x33 + fabs(s)*0x18);
            waterreflectionvertices.add({x, y, z, {255, 255, 255, alpha}, x+duv, y+duv, z});
            if(refract) waterrefractionvertices.add({x, y, z, {255, 255, 255, alpha}, x-duv, y-duv, z});
        };
        int start = 0;
        loopv(waterruns)
        {
            const WaterRun &run = waterruns[i];
            const int length = (run.y1 - run.y0) / watersubdiv + 1;
            const int count = waterreflectionvertices.length() - start;
            if(count && count + 2 + length * 2 > 8190)
            {
                start = waterreflectionvertices.length();
                waterbatchends.add(start);
            }
            if(waterreflectionvertices.length() > start)
            {
                // Duplicate the last and first vertices without recomputing
                // the shared wave coordinates at the strip join.
                const ProjectedWaterVertex last = waterreflectionvertices.last();
                waterreflectionvertices.add(last);
                if(refract)
                {
                    const ProjectedWaterVertex lastRefracted = waterrefractionvertices.last();
                    waterrefractionvertices.add(lastRefracted);
                }
                emit(run.x, run.y0);
            }
            for(int y = run.y0; y <= run.y1; y += watersubdiv)
            {
                emit(run.x, y);
                emit(run.x + watersubdiv, y);
            }
            projectedwaterquads += length / 2;
        }
        if(waterreflectionvertices.length() > start) waterbatchends.add(waterreflectionvertices.length());
        return true;
    }

    void drawprojectedwater(const vector<ProjectedWaterVertex> &vertices)
    {
        int start = 0;
        loopv(waterbatchends)
        {
            const int end = waterbatchends[i];
            ac3ds_gl_draw_projected_strip(vertices.getbuf()+start, end-start);
            start = end;
        }
        nquads += projectedwaterquads;
    }
}

#define RENDERWATER3DS(vertw, hf, t) if(!minimap) { drawwaterruns<vertw>(hf, t); } else
#else
#define RENDERWATER3DS(vertw, hf, t)
#endif

#define renderwaterstrips(vertw, hf, t) \
    def##vertw(); \
    RENDERWATER3DS(vertw, hf, t) \
    for(int x = wx1; x<wx2; x += watersubdiv) \
    { \
        varray::begin(GL_TRIANGLE_STRIP); \
        vertw(x,             wy1, hf, t); \
        vertw(x+watersubdiv, wy1, hf, t); \
        for(int y = wy1; y<wy2; y += watersubdiv) \
        { \
            vertw(x,             y+watersubdiv, hf, t); \
            vertw(x+watersubdiv, y+watersubdiv, hf, t); \
        } \
        nquads += varray::end()/4; \
    }

void setprojtexmatrix()
{
    glmatrixf projtex = mvpmatrix;
    projtex.projective();

    glMatrixMode(GL_TEXTURE);
    glLoadMatrixf(projtex.v);
}

void setupmultitexrefract(GLuint reflecttex, GLuint refracttex)
{
    setuptmu(0, "K , T @ Ka");

    colortmu(0, hdr.watercolor[0]/255.0f, hdr.watercolor[1]/255.0f, hdr.watercolor[2]/255.0f, hdr.watercolor[3]/255.0f);

    glBindTexture(GL_TEXTURE_2D, refracttex);
    setprojtexmatrix();

    glActiveTexture_(GL_TEXTURE1_ARB);
    glEnable(GL_TEXTURE_2D);

    setuptmu(1, "P , T @ C~a");

    glBindTexture(GL_TEXTURE_2D, reflecttex);
    setprojtexmatrix();

    glActiveTexture_(GL_TEXTURE0_ARB);
}

void setupmultitexreflect(GLuint reflecttex)
{
    setuptmu(0, "T , K @ Ca", "Ka * P~a");

    float a = hdr.watercolor[3]/255.0f;
    colortmu(0, hdr.watercolor[0]/255.0f*a, hdr.watercolor[1]/255.0f*a, hdr.watercolor[2]/255.0f*a, 1.0f-a);

    glBindTexture(GL_TEXTURE_2D, reflecttex);
    setprojtexmatrix();
}

void cleanupmultitex(GLuint reflecttex, GLuint refracttex)
{
    resettmu(0);
    glLoadIdentity();

    if(refracttex)
    {
        glActiveTexture_(GL_TEXTURE1_ARB);
        glDisable(GL_TEXTURE_2D);
        resettmu(1);
        glLoadIdentity();
        glActiveTexture_(GL_TEXTURE0_ARB);
    }
    glMatrixMode(GL_MODELVIEW);
}

VARP(mtwater, 0, 1, 1);

int renderwater(float hf, GLuint reflecttex, GLuint refracttex)
{
    if(wx1<0) return nquads;

    effective_waveheight = waterwaveheight();

    wx1 -= wx1%watersubdiv;
    wy1 -= wy1%watersubdiv;

#ifdef __3DS__
    if(!minimap) buildwaterruns(hf);
#endif

    float t = fmod(float(lastmillis/300.0f/PI2), 1.0f);

    if(mtwater && maxtmus>=2 && reflecttex)
    {
        varray::enable();
#ifdef __3DS__
        const bool direct = !minimap && ac3ds_gl_can_draw_projected_strip() &&
                            buildprojectedwater(hf, t, refracttex != 0);
#endif

        if(refracttex)
        {
#ifdef __3DS__
            // PICA only divides projected texture coordinates on unit zero.
            // Draw the same combiner equation as two unit-zero passes, so both
            // distortion directions retain perspective-correct sampling.
            setuptmu(0, "K , T @ Ka");
            colortmu(0, hdr.watercolor[0]/255.0f, hdr.watercolor[1]/255.0f,
                     hdr.watercolor[2]/255.0f, hdr.watercolor[3]/255.0f);
            glBindTexture(GL_TEXTURE_2D, refracttex);
            setprojtexmatrix();
            int originalquads = nquads;
            if(direct) drawprojectedwater(waterrefractionvertices);
            else { renderwaterstrips(vertwtc_refract, hf, t); }
            nquads = originalquads;

            setuptmu(0, "= T", "= Ca");
            glBindTexture(GL_TEXTURE_2D, reflecttex);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_FALSE);
            if(direct) drawprojectedwater(waterreflectionvertices);
            else { renderwaterstrips(vertwtc, hf, t); }
            glDepthMask(GL_TRUE);
            glDepthFunc(GL_LESS);
            glDisable(GL_BLEND);
#else
            setupmultitexrefract(reflecttex, refracttex);
            renderwaterstrips(vertwmtc, hf, t);
#endif
        }
        else
        {
            setupmultitexreflect(reflecttex);
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_SRC_ALPHA);
#ifdef __3DS__
            if(direct) drawprojectedwater(waterreflectionvertices);
            else
#endif
            { renderwaterstrips(vertwtc, hf, t); }
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }
#ifdef __3DS__
        cleanupmultitex(reflecttex, 0);
#else
        cleanupmultitex(reflecttex, refracttex);
#endif

        varray::disable();

        return nquads;
    }

    if(!refracttex)
    {
        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
    }
    glDepthMask(GL_FALSE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    varray::enable();

    if(reflecttex)
    {
        if(!refracttex)
        {
            glColor4ubv(hdr.watercolor);
            renderwaterstrips(vertw, hf, t);

            glEnable(GL_TEXTURE_2D);
        }

        setprojtexmatrix();

        glBindTexture(GL_TEXTURE_2D, refracttex ? refracttex : reflecttex);
    }

    if(refracttex)
    {
        glColor3f(1, 1, 1);
        renderwaterstrips(vertwt, hf, t);
        glEnable(GL_BLEND);

        glBindTexture(GL_TEXTURE_2D, reflecttex);
        glDepthMask(GL_TRUE);
    }
    if(reflecttex) { renderwaterstrips(vertwtc, hf, t); }
    else { renderwaterstrips(vertwc, hf, t); }

    varray::disable();

    if(reflecttex)
    {
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
    }
    else glEnable(GL_TEXTURE_2D);

    glDisable(GL_BLEND);
    if(!refracttex) glDepthMask(GL_TRUE);

    return nquads;
}

void addwaterquad(int x, int y, int size)       // update bounding rect that contains water
{
    int x2 = x+size;
    int y2 = y+size;
    if(wx1<0)
    {
        wx1 = x;
        wy1 = y;
        wx2 = x2;
        wy2 = y2;
    }
    else
    {
        if(x<wx1) wx1 = x;
        if(y<wy1) wy1 = y;
        if(x2>wx2) wx2 = x2;
        if(y2>wy2) wy2 = y2;
    }
}

#ifdef __3DS__
bool ac3ds_water_bounds(float level)
{
    if(wx1 < 0) return false;
    const int x0 = wx1 - wx1 % watersubdiv, y0 = wy1 - wy1 % watersubdiv;
    const int x1 = watergridend(x0, wx2), y1 = watergridend(y0, wy2);
    const float wave = waterwaveheight() * 0.5f;
    const WaterFrustum frustum(level, wave);
    WaterFrustum::Point polygon[12] = {{double(x0), double(y0)}, {double(x1), double(y0)},
                                       {double(x1), double(y1)}, {double(x0), double(y1)}};
    const int count = frustum.clip(polygon, 4);
    if(!count) return false;

    // Both the surface height and each projected UV displacement have
    // magnitude <= wave. Expand the clipped XY polygon by that amount
    // and precision margin on all axes. Each expanded vertex is a sum
    // of a polygon vertex and a box corner; any other point is a convex
    // combination of those sums. With W > 0 the projected coordinates
    // are also convex combinations (with weights multiplied by W).
    const float margin = wave + 0.125f;
    double sx1 = 1, sy1 = 1, sx2 = -1, sy2 = -1;
    loopi(count) loopj(8)
    {
        const double x = polygon[i].x + (j & 1 ? margin : -margin);
        const double y = polygon[i].y + (j & 2 ? margin : -margin);
        const double z = level + (j & 4 ? margin : -margin);
        const double w = x*mvpmatrix[3] + y*mvpmatrix[7] + z*mvpmatrix[11] + mvpmatrix[15];
        if(w <= 0.0001)
        {
            // Distortion can cross the camera plane even after clipping
            // the undistorted surface. Keep every texel in that case.
            wsx1 = wsy1 = -1.f;
            wsx2 = wsy2 = 1.f;
            return true;
        }
        const double px = (x*mvpmatrix[0] + y*mvpmatrix[4] + z*mvpmatrix[8] + mvpmatrix[12]) / w;
        const double py = (x*mvpmatrix[1] + y*mvpmatrix[5] + z*mvpmatrix[9] + mvpmatrix[13]) / w;
        sx1 = min(sx1, px);
        sy1 = min(sy1, py);
        sx2 = max(sx2, px);
        sy2 = max(sy2, py);
    }
    wsx1 = clamp(sx1, -1.0, 1.0);
    wsy1 = clamp(sy1, -1.0, 1.0);
    wsx2 = clamp(sx2, -1.0, 1.0);
    wsy2 = clamp(sy2, -1.0, 1.0);
    return true;
}
#endif

void calcwaterscissor()
{
    vec4 v[4];
    float sx1 = 1, sy1 = 1, sx2 = -1, sy2 = -1;
    loopi(4)
    {
        vec4 &p = v[i];
        mvpmatrix.transform(vec(i&1 ? wx2 : wx1, i&2 ? wy2 : wy1, waterlevel - 0.3f), p);
        if(p.z >= 0)
        {
            float x = p.x / p.w, y = p.y / p.w;
            sx1 = min(sx1, x);
            sy1 = min(sy1, y);
            sx2 = max(sx2, x);
            sy2 = max(sy2, y);
        }
    }
    if(sx1 >= sx2 || sy1 >= sy2)
    {
        sx1 = sy1 = -1;
        sx2 = sy2 = 1;
    }
    else loopi(4)
    {
        const vec4 &p = v[i];
        if(p.z >= 0) continue;
        loopj(2)
        {
            const vec4 &o = v[i^(1<<j)];
            if(o.z <= 0) continue;
            float t = p.z/(p.z - o.z),
                  w = p.w + t*(o.w - p.w),
                  x = (p.x + t*(o.x - p.x))/w,
                  y = (p.y + t*(o.y - p.y))/w;
            sx1 = min(sx1, x);
            sy1 = min(sy1, y);
            sx2 = max(sx2, x);
            sy2 = max(sy2, y);
        }
    }
    wsx1 = max(sx1, -1.0f);
    wsy1 = max(sy1, -1.0f);
    wsx2 = min(sx2, 1.0f);
    wsy2 = min(sy2, 1.0f);
}

void resetwater()
{
    if(!reflecting) wx1 = -1;
}

