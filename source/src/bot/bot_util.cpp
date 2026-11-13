//
// C++ Implementation: bot_util
//
// Description: Basic util functions, mostly bot related
//
//
// Author:  <rickhelmus@gmail.com>
//
//
//
//

#include "cube.h"
#include "bot.h"

// Function code by PMB - Begin


// Random functions begin
// copied from
// http://forums.bots-united.com/showthread.php?t=244&page=1&pp=10&highlight=random
// So credits to PMB and the person(s) who actually made it :]

// maximum value returned by our number generator (2^31 - 1 = 0x7FFFFFFF)
#define LRAND_MAX 2147483647L
long lseed; // our random number generator's seed

long lrand (void)
{
     // this function is the equivalent of the rand() standard C library function,
     // except that whereas rand() works only with short integers
     // (i.e. not above 32767), this function is able to generate 32-bit random
     // numbers. Isn't that nice ?
     // credits go to Ray Gardner for his fast implementation of minimal random
     // number generators
     // http://c.snippets.org/snip_lister.php?fname=rg_rand.c

     // compose the two 16-bit parts of the long integer and assemble them
     static unsigned long lrand_lo, lrand_hi;
     lrand_lo = 16807 * (long) (lseed & 0xFFFF); // low part
     lrand_hi = 16807 * (long) ((unsigned long) lseed >> 16);
     // high part
     lrand_lo += (lrand_hi & 0x7FFF) << 16;
     // assemble both in lrand_lo
     // is the resulting number greater than LRAND_MAX (half the capacity) ?
     if (lrand_lo > LRAND_MAX)
     {
          lrand_lo &= LRAND_MAX; // then get rid of the disturbing bit
          lrand_lo++; // and increase it a bit (to avoid overflow problems, I suppose)
     }

     lrand_lo += lrand_hi >> 15;
     // now do twisted maths to generate the next seed
     // is the resulting number greater than LRAND_MAX (half the capacity) ?
     if (lrand_lo > LRAND_MAX)
     {
          lrand_lo &= LRAND_MAX; // then get rid of the disturbing bit
          lrand_lo++; // and increase it a bit (to avoid overflow problems, I suppose)
     }
     // now we've got our (pseudo-)random number.
     lseed = (long) lrand_lo; // put it in the seed for next time
     return (lseed); // and return it. Yeah, simple as that.
}

void lsrand (unsigned long initial_seed)
{
     // this function initializes the random seed based on the initial seed value
     // passed in the initial_seed parameter. Since random seeds are
     // usually initialized with the time of day, and time is a value that
     // changes slowly, we bump the seed twice to have it in a more
     // random state for future calls of the random number generator.

     // fill in the initial seed of the random number generator
     lseed = (long) initial_seed;
     lseed = lrand (); // bump it once
     lseed = lrand (); // bump it twice
     return; // that's all folks
}

long RandomLong (long from, long to)
{
     // this function returns a random integer number between (and including)
     // the starting and ending values passed by parameters from and to.
     if (to <= from)
          return (from);

     return (from + lrand () / (LRAND_MAX / (to - from + 1)));
}

float RandomFloat (float from, float to)
{
     // this function returns a random floating-point number between (and including)
     // the starting and ending values passed by parameters from and to.

     if (to <= from)
          return (from);

     return (from + (float) lrand () / ((float) LRAND_MAX / (to - from)));
}

// End random functions

void AnglesToVectors(vec angles, vec &forward, vec &right, vec &up)
{
     float angle;
     float sin_pitch;
     float sin_yaw;
     float sin_roll;
     float cos_pitch;
     float cos_yaw;
     float cos_roll;

     // For some reason this has to be done :)
     // Me = math n00b
     angles.x = -angles.x;
     angles.y -= 90;

     // compute the sin and cosine of the pitch component
     angle = angles.x * RAD;
     sin_pitch = sinf (angle);
     cos_pitch = cosf (angle);

     // compute the sin and cosine of the yaw component
     angle = angles.y * RAD;
     sin_yaw = sinf (angle);
     cos_yaw = cosf (angle);

     // compute the sin and cosine of the roll component
     angle = angles.z * RAD;
     sin_roll = sinf (angle);
     cos_roll = cosf (angle);

     // build the FORWARD vector
     forward.x = cos_pitch * cos_yaw;
     forward.y = cos_pitch * sin_yaw;
     forward.z = -sin_pitch;

     // build the RIGHT vector
     right.x = -(-(sin_roll * sin_pitch * cos_yaw) - (cos_roll * -sin_yaw));
     right.y = -(-(sin_roll * sin_pitch * sin_yaw) - (cos_roll * cos_yaw));
     right.z = -(sin_roll * cos_pitch);

     // build the UPWARDS vector
     up.x = ((cos_roll * sin_pitch * cos_yaw) - (sin_roll * -sin_yaw));
     up.y = ((cos_roll * sin_pitch * sin_yaw) - (sin_roll * cos_yaw));
     up.z = cos_roll * cos_pitch;

     return;
}

// Function code by PMB - End

float WrapXAngle(float angle)
{
     if (angle > 90) angle = 90;
     else if (angle < -90) angle = -90;

     return angle;
}

float WrapYZAngle(float angle)
{
     while (angle >= 360.0f) angle -= 360.0f;
     while (angle < 0.0f) angle += 360.0f;

     return angle;
}

#ifdef __3DS__
struct BotTraceMapmodel
{
     int entityIndex;
     const mapmodelinfo *model;
     float minx, miny, maxx, maxy;
};

static vector<BotTraceMapmodel> botTraceMapmodels;
static bool botTraceMapmodelsActive = false;

// Ground probes normally touch one coarse cell. Keep a bitset for maps
// with up to 64 collision models and keep ordered scanning for larger maps.
enum { BotTraceGridShift = 5, BotTraceGridSize = 1 << (LARGEST_FACTOR-BotTraceGridShift) };
struct BotTraceCell
{
     unsigned generation;
     uint64_t models;
};
static BotTraceCell botTraceGrid[BotTraceGridSize*BotTraceGridSize];
static unsigned botTraceGridGeneration;
static bool botTraceGridActive;

static int botTraceCell(float coordinate)
{
     const int cellSize = 1 << BotTraceGridShift;
     return clamp(int(coordinate/cellSize), 0, BotTraceGridSize-1);
}

static void prepareBotTraceGrid()
{
     if(!++botTraceGridGeneration)
     {
          memset(botTraceGrid, 0, sizeof(botTraceGrid));
          ++botTraceGridGeneration;
     }
     loopv(botTraceMapmodels)
     {
          const BotTraceMapmodel &box = botTraceMapmodels[i];
          const int x0 = botTraceCell(box.minx), y0 = botTraceCell(box.miny);
          const int x1 = botTraceCell(box.maxx), y1 = botTraceCell(box.maxy);
          for(int y = y0; y <= y1; ++y) for(int x = x0; x <= x1; ++x)
          {
               BotTraceCell &cell = botTraceGrid[y*BotTraceGridSize + x];
               if(cell.generation != botTraceGridGeneration) cell = {botTraceGridGeneration, 0};
               cell.models |= uint64_t(1) << i;
          }
     }
}

void beginBotMapmodelTraces()
{
     botTraceMapmodels.setsize(0);
     // The entity list and model slots stay fixed during the bot update.
     // Trace callers outside that update continue to read the current map.
     loopv(ents)
     {
          const entity &e = ents[i];
          if(e.type != MAPMODEL) continue;
          const mapmodelinfo *model = getmminfo(e.attr2);
          if(!model || !model->h || model->rad < 0) continue;
          const float radius = model->rad;
          botTraceMapmodels.add({i, model, e.x-radius, e.y-radius, e.x+radius, e.y+radius});
     }
     botTraceGridActive = !botTraceMapmodels.empty() && botTraceMapmodels.length() <= 64;
     if(botTraceGridActive) prepareBotTraceGrid();
     botTraceMapmodelsActive = true;
}

void endBotMapmodelTraces()
{
     botTraceMapmodelsActive = false;
}
#endif

static void traceBotMapmodel(entity &e, const mapmodelinfo &model, const vec &from, const vec &to,
                             traceresult_s &tr, float &nearest)
{
     const float lo = float(S(e.x, e.y)->floor + model.zoff + e.attr3);
     const float hi = lo + model.h;
     const float z = fabs(from.z-lo) < fabs(from.z-hi) ? lo : hi;
     if(GetDistance(from, vec(e.x, e.y, z)) < nearest && intersect(&e, from, to, &tr.end))
     {
          nearest = GetDistance(from, tr.end);
          tr.collided = true;
     }
}

#ifdef __3DS__
static void traceBotMapmodels(const vec &from, const vec &to, traceresult_s &tr, float &nearest)
{
     // The legacy intersection samples a point on the segment. Leave room
     // for its float interpolation to round past an endpoint.
     constexpr float margin = 0.0625f;
     const float minx = min(from.x, to.x)-margin, maxx = max(from.x, to.x)+margin;
     const float miny = min(from.y, to.y)-margin, maxy = max(from.y, to.y)+margin;
     const auto trace = [&](const BotTraceMapmodel &box)
     {
          if(box.maxx < minx || box.maxy < miny || box.minx > maxx || box.miny > maxy) return;
          traceBotMapmodel(ents[box.entityIndex], *box.model, from, to, tr, nearest);
     };
     if(botTraceGridActive)
     {
          const int x0 = botTraceCell(minx), y0 = botTraceCell(miny);
          const int x1 = botTraceCell(maxx), y1 = botTraceCell(maxy);
          if(x1-x0 <= 1 && y1-y0 <= 1)
          {
               uint64_t candidates = 0;
               for(int y = y0; y <= y1; ++y) for(int x = x0; x <= x1; ++x)
               {
                    const BotTraceCell &cell = botTraceGrid[y*BotTraceGridSize + x];
                    if(cell.generation == botTraceGridGeneration) candidates |= cell.models;
               }
               while(candidates)
               {
                    trace(botTraceMapmodels[__builtin_ctzll(candidates)]);
                    candidates &= candidates-1;
               }
               return;
          }
     }
     loopv(botTraceMapmodels) trace(botTraceMapmodels[i]);
}
#endif

static void traceBotEntities(const vec &from, const vec &to, traceresult_s &tr, float &nearest)
{
#ifdef __3DS__
     if(botTraceMapmodelsActive && fabsf(to.x) <= 32768.0f && fabsf(to.y) <= 32768.0f)
     {
          traceBotMapmodels(from, to, tr, nearest);
          return;
     }
#endif
     loopv(ents)
     {
          entity &e = ents[i];
          if(e.type != MAPMODEL) continue;
          const mapmodelinfo *model = getmminfo(e.attr2);
          if(model && model->h) traceBotMapmodel(e, *model, from, to, tr, nearest);
     }
}

template<bool visibilityOnly>
static void traceBotWorld(const vec &from, const vec &to, traceresult_s *tr,
                          float flNearestDist, bool SkipTags)
{
     bool solid;
     float dx = to.x-from.x;
     float dy = to.y-from.y;
     int steps = (int)(sqrt(dx*dx+dy*dy)/0.9);
     if(!steps) // If from and to are on the same cube...
     {
          if (visibilityOnly || GetDistance(from, to) < flNearestDist)
          {
               tr->end = to;
               sqr *s = S(int(from.x), int(from.y));
               float flr = GetCubeFloor(int(from.x), int(from.y));
               solid = ((!SkipTags || !s->tag) && SOLID(s));
               if (solid || (to.z < flr) || (to.z > s->ceil))
               {
                    tr->collided = true;
                    tr->end.z = (fabs(to.z-s->ceil) < fabs(to.z-flr)) ? s->ceil : flr;
               }
          }
          return;
     }

     float x = from.x;
     float y = from.y;
     int i = 0;
     vec endcube = from;
     const float xstep = dx/(float)steps, ystep = dy/(float)steps;
     const float zdelta = from.z-to.z;

#ifdef __3DS__
     // A segment between nearby heights cannot reach the 9999-unit
     // distance sentinel while its sampled positions remain in the map.
     const bool checkDistance = !visibilityOnly && (tr->collided || !(fabsf(to.z-from.z) < 8000.0f));
#else
     const bool checkDistance = true;
#endif

     // Now check if the 'line' is hit by a cube
     while(i<steps)
     {
          if(OUTBORD((int)x, (int)y)) break;
          if (checkDistance && GetDistance(from, endcube) >= flNearestDist) break;
          sqr *s = S(int(x), int(y));
          solid = ((!SkipTags || !s->tag) && SOLID(s));
          if(solid) break;
          float floor = s->floor;
          if(s->type==FHF) floor -= s->vdelta/4.0f;
          float ceil = s->ceil;
          if(s->type==CHF) ceil += s->vdelta/4.0f;
          float rz = from.z-(zdelta*(i/(float)steps));
          if(rz<floor || rz>ceil) break;

          if(!visibilityOnly) endcube = vec(x, y, rz);
          x += xstep;
          y += ystep;
          i++;
     }

     if ((i>=steps) && !tr->collided)
     {
          tr->end = to;
     }
     else
     {
          tr->collided = true;
          if (!checkDistance || GetDistance(from, endcube) < flNearestDist)
               tr->end = endcube;
     }

     return;
}

// Find the first collision and its endpoint.
void TraceLine(vec from, vec to, dynent *pTracer, bool CheckPlayers, traceresult_s *tr,
               bool SkipTags)
{
     tr->end = from;
     tr->collided = false;
     if(OUTBORD((int)from.x, (int)from.y)) return;

     float flNearestDist = 9999.0f;
     traceBotEntities(from, to, *tr, flNearestDist);
     if (CheckPlayers)
     {
          loopv(bots)
          {
               playerent *d = bots[i];
               if(!d || (d==pTracer) || (d->state != CS_ALIVE)) continue;

               float flDist = GetDistance(from, d->o);
               if ((flDist < flNearestDist) && (intersect(d, from, to, &tr->end)))
               {
                    flNearestDist = flDist;
                    tr->collided = true;
               }
          }

          playerent *d = player1;
          if (d && (d!=pTracer) && !BotManager.m_pBotToView && (d->state == CS_ALIVE))
          {
               float flDist = GetDistance(from, d->o);
               if ((flDist < flNearestDist) && (intersect(d, from, to, &tr->end)))
               {
                    flNearestDist = flDist;
                    tr->collided = true;
               }
          }
     }
     traceBotWorld<false>(from, to, tr, flNearestDist, SkipTags);
}

float botGroundDistance(const vec &from)
{
     const vec down(from.x, from.y, from.z-100.0f);
     traceresult_s hit;
     TraceLine(from, down, NULL, false, &hit);
     return GetDistance(from, hit.end);
}

float GetDistance(vec v1, vec v2)
{
     if ((v1.x == 0) && (v2.x == 0) && (v1.y == 0) && (v2.y == 0) && (v1.z == 0) &&
         (v2.z == 0))
          return 0.0f;

     return v1.dist(v2);
}

// As GetDistance(), but doesn take height in account.
float Get2DDistance(vec v1, vec v2)
{
     if ((v1.x == 0) && (v2.x == 0) && (v1.y == 0) && (v2.y == 0))
          return 0.0f;

     v1.z = v2.z = 0.0f;
     return v1.dist(v2);
}

bool IsVisible(vec v1, vec v2, dynent *tracer, bool SkipTags)
{
     traceresult_s tr;
#ifdef __3DS__
     if(!tracer && fabsf(v2.z-v1.z) < 8000.0f)
     {
          if(OUTBORD((int)v1.x, (int)v1.y)) return true;
          // A collision from either source blocks the line of sight.
          tr.collided = false;
          traceBotWorld<true>(v1, v2, &tr, 9999.0f, SkipTags);
          if(tr.collided) return false;
          float nearest = 9999.0f;
          traceBotEntities(v1, v2, tr, nearest);
          return !tr.collided;
     }
#endif
     TraceLine(v1, v2, tracer, (tracer!=NULL), &tr, SkipTags);
     return !tr.collided;
}

// Prediction:
// - pos: Current position
// - vel: Current velocity
// - Time: In seconds, predict how far it is in Time seconds
vec PredictPos(vec pos, vec vel, float Time)
{
     float flVelLength = vel.magnitude();

     if (flVelLength <= 1.0)
          return pos; // don't bother with low velocities...

     vec v_end = vel;
     v_end.mul(Time);
     v_end.add(pos);
     return v_end;
}

// returns true if the given file is valid
bool IsValidFile(const char *szFileName)
{
     FILE *fp = fopen(szFileName, "r");

     if (fp)
     {
          fclose(fp);
          return true;
     }

     return false;
}

bool FileIsOlder(const char *szFileName1, const char *szFileName2)
{
/*     int file1, file2;
     struct stat stat1, stat2;

     file1 = open(szFileName1, O_RDONLY);
     file2 = open(szFileName2, O_RDONLY);

     fstat(file1, &stat1);
     fstat(file2, &stat2);

     close(file1);
     close(file2);

     return(stat1.st_mtime < stat2.st_mtime);*/ return false; // FIXME
}

vec Normalize(vec v)
{
     float flLen = sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
     static vec v_norm = g_vecZero;
     if (flLen == 0)
     {
          v_norm.x=0;v_norm.y=0;v_norm.z=1;
     }
     else
     {
          flLen = 1 / flLen;
          v_norm.x = v.x * flLen; v_norm.y = v.y * flLen; v_norm.z = v.z * flLen;
     }
     return v_norm;
}

float GetYawDiff(float curyaw, vec v1, vec v2)
{
     float yaw = -(float)atan2(v2.x - v1.x, v2.y - v1.y)/PI*180+180;
     return yaw-curyaw;
}

vec CrossProduct(const vec &a, const vec &b)
{
     vec cross(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
     return cross;
}

int GetDirection(const vec &angles, const vec &v1, const vec &v2)
{
     vec tmp, forward, right, up, cross;
     float flDot;

     AnglesToVectors(angles, forward, right, up);

     tmp = v2;
     tmp.sub(v1);
     tmp.z = 0.0f; // Make 2D
     tmp = Normalize(tmp);
     forward.z = 0; // Make 2D

     flDot = tmp.dot(forward);
     cross = CrossProduct(tmp, forward);

     int Dir = DIR_NONE;

     if (cross.z > 0.1f) Dir |= LEFT;
     else if (cross.z < -0.1f) Dir |= RIGHT;
     if (flDot < -0.1f) Dir |= BACKWARD;
     else if (flDot > 0.1f) Dir |= FORWARD;

     if (v1.z > v2.z) Dir |= DOWN;
     if (v1.z < v2.z) Dir |= UP;

     return Dir;
}

float GetCubeFloor(int x, int y)
{
     sqr *s = S(x, y);
     float floor = s->floor;
     if (s->type == FHF)
          floor -= (s->vdelta+S(x+1,y)->vdelta+S(x,y+1)->vdelta+S(x+1,y+1)->vdelta)/16.0f;
     return floor;
}

float GetCubeHeight(int x, int y)
{
     sqr *s = S(x, y);
     float ceil = s->ceil;
     if(s->type == CHF)
          ceil += (s->vdelta+S(x+1,y)->vdelta+S(x,y+1)->vdelta+S(x+1,y+1)->vdelta)/16.0f;
     return ceil;
}

const char *SkillNrToSkillName(short skillnr)
{
     if (skillnr == 0) return "best";
     else if (skillnr == 1) return "good";
     else if (skillnr == 2) return "medium";
     else if (skillnr == 3) return "worse";
     else if (skillnr == 4) return "bad";

     return NULL;
}

bool IsInGame(dynent *d)
{
     if (!d) return false;

     if (d->type==ENT_BOT)
     {
          loopv(bots)
          {
               if (bots[i] == d)
                    return true;
          }
     }
     else
     {
          if (d == player1)
               return true;
     }
     return false;
}
