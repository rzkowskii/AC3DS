#ifndef BOT_UTIL_H
#define BOT_UTIL_H

// Structure for storing traceresults
struct traceresult_s
{
     vec end;
     bool collided;
};

long RandomLong(long from, long to);
float RandomFloat(float from, float to);
void lsrand(unsigned long initial_seed);

void AnglesToVectors(vec angles, vec &forward, vec &right, vec &up);
float WrapXAngle(float angle);
float WrapYZAngle(float angle);
void TraceLine(vec from, vec to, dynent *pTracer, bool CheckPlayers, traceresult_s *tr,
               bool SkipTags=false);
#ifdef __3DS__
void beginBotMapmodelTraces();
void endBotMapmodelTraces();
#endif
float botGroundDistance(const vec &from);
float GetDistance(vec v1, vec v2);
float Get2DDistance(vec v1, vec v2);
bool IsVisible(vec v1, vec v2, dynent *tracer = NULL, bool SkipTags=false);
bool IsValidFile(const char *szFileName);
bool FileIsOlder(const char *szFileName1, const char *szFileName2);
vec PredictPos(vec pos, vec vel, float Time);
vec Normalize(vec v);
inline void makevec(vec *v, float x, float y, float z) { v->x=x; v->y=y; v->z=z; }
inline bool UnderWater(const vec &o) { return waterlevel > o.z - 0.5f; }
inline bool InWater(const vec &o) { return waterlevel >= o.z; }
float GetYawDiff(float curyaw, vec v1, vec v2);
vec CrossProduct(const vec &a, const vec &b);
int GetDirection(const vec &angles, const vec &v1, const vec &v2);
float GetCubeFloor(int x, int y);
float GetCubeHeight(int x, int y);
const char *SkillNrToSkillName(short skillnr);
bool IsInGame(dynent *d);

// ==================================================================
// Code of TLinkedList - Start
// ==================================================================

template <class C> class TLinkedList
{
public:
     struct node_s
     {
          C Entry;
          node_s *next;
          node_s *prev;

          node_s(void) : next(NULL), prev(NULL)
          {
               //memset(&Entry, 0, sizeof(Entry));
          };
     };

     // member functions

     void AddNode(C entry)
     {
          if (!pNodeList)
          {
               pNodeList = new node_s;
               pNodeList->Entry = entry;
               pNodeList->next = NULL;
               pNodeList->prev = NULL;
               pLastNode = pNodeList;
               iNodeCount = 1;
          }
          else
          {
               pLastNode->next = new node_s;
               pLastNode->next->prev = pLastNode;
               pLastNode = pLastNode->next;
               pLastNode->Entry = entry;
               pLastNode->next = NULL;
               iNodeCount++;
          }
     }

     void PushNode(C Entry)
     {
          if (!pNodeList)
          {
               pNodeList = new node_s;
               pNodeList->Entry = Entry;
               pNodeList->next = NULL;
               pNodeList->prev = NULL;
               pLastNode = pNodeList;
               iNodeCount = 1;
          }
          else
          {
               node_s *pNew = new node_s;
               pNew->Entry = Entry;
               pNew->prev = NULL;
               pNew->next = pNodeList;
               pNodeList->prev = pNew;
               pNodeList = pNew;
               iNodeCount++;
          }
     }

     void DeleteEntry(C Entry)
     {
          node_s *pNode = pNodeList;
          if (!pNode)
               return;

          if (pNode->Entry == Entry) // first node
          {
               if (pNodeList == pLastNode)
                    pLastNode = NULL;

               pNodeList = pNodeList->next;
               if (pNodeList)
                    pNodeList->prev = NULL;
               pNode->next = NULL;
               delete pNode;
               pNode = NULL;
               iNodeCount--;
               return;
          }

          if (Entry == pLastNode->Entry) // last node
          {
               pNode = pLastNode;
               pLastNode = pLastNode->prev;
               pLastNode->next = NULL;

               pNode->next = NULL;
               pNode->prev = NULL;
               delete pNode;
               pNode = NULL;
               iNodeCount--;
               return;
          }

          // node is somewhere in the middle
          pNode = SearchNode(Entry);

          if (!pNode)
               return;

          node_s *pPrevNode = pNode->prev;

          if (!pPrevNode)
               return;

          // unlink pNode
          pNode->next->prev = pPrevNode;
          pPrevNode->next = pNode->next;

          pNode->next = NULL;
          pNode->prev = NULL;
          delete pNode;
          pNode = NULL;
          iNodeCount--;
     }

     void DeleteNode(node_s *pNode)
     {
          if (!pNode)
               return;

          if (pNodeList == pNode) // first node
          {
               if (pNodeList == pLastNode)
                    pLastNode = pNodeList->next;

               pNodeList = pNodeList->next;
               if (pNodeList)
                    pNodeList->prev = NULL;
               pNode->next = NULL;
               delete pNode;
               pNode = NULL;
               iNodeCount--;
               return;
          }

          if (pNode == pLastNode) // last node
          {
               pNode = pLastNode;
               pLastNode = pLastNode->prev;
               pLastNode->next = NULL;

               pNode->next = NULL;
               pNode->prev = NULL;
               delete pNode;
               pNode = NULL;
               iNodeCount--;
               return;
          }

          // node is somewhere in the middle

          node_s *pPrevNode = pNode->prev;

          if (!pPrevNode)
               return;

          // unlink pNode
          pNode->next->prev = pPrevNode;
          pPrevNode->next = pNode->next;

          pNode->next = NULL;
          pNode->prev = NULL;
          delete pNode;
          pNode = NULL;
          iNodeCount--;
     }

     void DeleteAllNodes(void)
     {
          node_s *pNode = pNodeList;
          node_s *pTemp;
          while (pNode != NULL)
          {
               pTemp = pNode;
               pNode = pNode->next;
               pTemp->next = NULL;
               pTemp->prev = NULL;
               delete pTemp;
          }
          pNodeList = pLastNode = NULL;
          iNodeCount = 0;
     }

     void Reset(void) // Special case, doesn't delete existing nodes
     {
          pNodeList = pLastNode = NULL;
          iNodeCount = 0;
     }

     node_s *SearchNode(C Entry)
     {
          node_s *pNode = pNodeList;
          while(pNode)
          {
               if (pNode->Entry == Entry)
                    return pNode;
               pNode = pNode->next;
          }
          return NULL;
     }

     C Pop(void)
     {
          if (!pNodeList)
               return static_cast<C>(NULL);

          C Entry = pNodeList->Entry;
          DeleteNode(pNodeList);
          return Entry;
     }

     node_s *GetFirst(void) {
        return pNodeList;
     };
     node_s *GetLast(void) { return pLastNode; };

     void EditEntry(C OrigEntry, C NewVal)
     {
          node_s *pNode = SearchNode(OrigEntry);

          if (pNode)
               pNode->Entry = NewVal;
     }

     bool IsInList(C Entry)
     {
          return (SearchNode(Entry) != NULL);
     }

     bool Empty(void)          { return (pNodeList == NULL); };
     int NodeCount(void)      { return iNodeCount; };


     // construction/destruction
     TLinkedList(void)
     {
          pNodeList = NULL;
          pLastNode = NULL;
          iNodeCount = 0;
     };

     ~TLinkedList(void)
     {
          DeleteAllNodes();
     };

//fixmebot
//private:
     node_s *pNodeList;
     node_s *pLastNode;
     int iNodeCount;
};

// ==================================================================
// Code of TLinkedList - End
// ==================================================================

// ==================================================================
// Code of TPriorList - Begin
// ==================================================================

template <class C, class D=int> class TPriorList
{
public:
     struct node_s
     {
          C Entry;
          D Priority;
          node_s *next;

          node_s(C Ent, D Prior) : Entry(Ent), Priority(Prior), next(NULL) {};
     };

     TPriorList(void) : pHeadNode(NULL), pLastNode(NULL) {};
     ~TPriorList(void) { Clear(); };

     void AddEntry(C Entry, D Prior)
     {
          if (!pHeadNode)
          {
               pHeadNode = new node_s(Entry, Prior);
               pLastNode = pHeadNode;
               iNodeCount=1;
          }
          else
          {
               iNodeCount++;
               node_s *pNew = new node_s(Entry, Prior);
               node_s *pNode = pHeadNode;
               node_s *pPrev = NULL;

               while(pNode)
               {
                    if (Prior < pNode->Priority)
                    {
                         if (!pPrev)
                         {
                              pNew->next = pNode;
                              pHeadNode = pNew;
                         }
                         else
                         {
                              pPrev->next = pNew;
                              pNew->next = pNode;
                         }
                         break;
                    }
                    pPrev = pNode;
                    pNode = pNode->next;
               }

               if (!pNode)
               {
                    pLastNode = pNew;
                    if (pPrev)
                         pPrev->next = pNew;
               }
          }
     }

     C Pop(void)
     {
          if (!pHeadNode)
               return static_cast<C>(NULL);

          C Entry = pHeadNode->Entry;
          DeleteNode(pHeadNode);
          return Entry;
     }

     void Clear(void)
     {
          node_s *pTemp;
          while(pHeadNode)
          {
               pTemp = pHeadNode;
               pHeadNode = pHeadNode->next;
               delete pTemp;
          }

          pHeadNode = pLastNode = NULL;
     }

     bool IsInList(C Entry, D Prior)
     {
          node_s *pNode = pHeadNode;
          while(pNode)
          {
               if (pNode->Entry == Entry)
                    return true;

               if (Prior < pNode->Priority)
                    return false;

               pNode = pNode->next;
          }

          return false;
     }

     bool Empty(void) { return (pHeadNode == NULL); }
     node_s *GetFirst(void) { return pHeadNode; }
private:
     node_s *pHeadNode;
     node_s *pLastNode;
     int iNodeCount;

     node_s *GetPrevNode(node_s *pNode)
     {
          node_s *pTemp = pHeadNode;
          while(pTemp)
          {
               if (pTemp->next == pNode)
                    return pTemp;
               pTemp = pTemp->next;
          }

          return NULL;
     }

     void DeleteNode(node_s *pNode)
     {
          if (!pNode)
               return;

          if (pHeadNode == pNode) // first node
          {
               if (pHeadNode == pLastNode)
                    pLastNode = pHeadNode->next;

               pHeadNode = pHeadNode->next;
               pNode->next = NULL;
               delete pNode;
               pNode = NULL;
               iNodeCount--;
               return;
          }

          if (pNode == pLastNode) // last node
          {
               pNode = pLastNode;
               pLastNode->next = NULL;

               pNode->next = NULL;
               delete pNode;
               pNode = NULL;
               iNodeCount--;
               return;
          }

          // node is somewhere in the middle

          node_s *pPrevNode = GetPrevNode(pNode);
          if (!pPrevNode)
               return;

          // unlink pNode
          pPrevNode->next = pNode->next;
          pNode->next = NULL;
          delete pNode;
          pNode = NULL;
          iNodeCount--;
     }
};

#ifdef __3DS__
template<class C> class BotWaypointQueue
{
     struct Entry { C node; float priority; unsigned order; };
     vector<Entry> entries;
     unsigned nextOrder = 0;
     bool ordered = false;

     static bool before(const Entry &a, const Entry &b)
     {
          if(a.priority < b.priority) return true;
          if(b.priority < a.priority) return false;
          return a.order < b.order;
     }

     static int compare(const Entry *a, const Entry *b)
     {
          if(before(*a, *b)) return -1;
          return before(*b, *a) ? 1 : 0;
     }

public:
     bool Empty() const { return entries.empty(); }

     void Clear()
     {
          entries.setsize(0);
          nextOrder = 0;
          ordered = false;
     }

     void AddEntry(C node, float priority)
     {
          const Entry entry = {node, priority, nextOrder++};
          if(!ordered && priority != priority)
          {
               // NaN is unordered. Restore the legacy linear insertion
               // rules for this search before adding an unordered priority.
               if(!entries.empty()) entries.sort(compare);
               ordered = true;
          }
          if(ordered)
          {
               int i = 0;
               while(i < entries.length() && !(priority < entries[i].priority)) ++i;
               entries.insert(i, entry);
               return;
          }

          int i = entries.length();
          entries.add(entry);
          while(i > 0)
          {
               const int parent = (i-1)/2;
               if(!before(entry, entries[parent])) break;
               entries[i] = entries[parent];
               i = parent;
          }
          entries[i] = entry;
     }

     C Pop()
     {
          if(entries.empty()) return NULL;
          const C result = entries[0].node;
          if(ordered)
          {
               entries.remove(0);
               return result;
          }
          const Entry entry = entries.pop();
          if(entries.empty()) return result;
          int i = 0;
          while(2*i+1 < entries.length())
          {
               int child = 2*i+1;
               if(child+1 < entries.length() && before(entries[child+1], entries[child])) ++child;
               if(!before(entries[child], entry)) break;
               entries[i] = entries[child];
               i = child;
          }
          entries[i] = entry;
          return result;
     }
};
#endif

// ==================================================================
// Code of TPriorList - End
// ==================================================================


// ==================================================================
// Code of TMultiChoice - Begin
// ==================================================================

template <class C> class TMultiChoice
{
     struct SMultiChoice
     {
          int MinVal;
          int MaxVal;
          C Choice;
          SMultiChoice(void) : MinVal(0), MaxVal(0){};
     };

     int TotalVal;
     TLinkedList<SMultiChoice*> *pChoiceList;

public:
     TMultiChoice(void) : TotalVal(0) // Constructor
     {
             pChoiceList = new TLinkedList<SMultiChoice*>;
     };

     ~TMultiChoice(void) // Destructor
     {
          while(pChoiceList->Empty() == false)
          {
               SMultiChoice *pEntry = pChoiceList->Pop();
               if (pEntry)
                    delete pEntry;
               else
                    break;
          }
          delete pChoiceList;
          pChoiceList = NULL;
     }

     void Insert(C Choice, short Percent = 50)
     {
          if (Percent == 0)
               return;

          SMultiChoice *pChoiceEntry = new SMultiChoice;

          pChoiceEntry->MinVal = TotalVal;
          pChoiceEntry->MaxVal = TotalVal + Percent;
          pChoiceEntry->Choice = Choice;

          pChoiceList->AddNode(pChoiceEntry);

          TotalVal += Percent;
     };

     bool FindSelection(SMultiChoice *MS, int Choice)
     {
          if ((Choice >= MS->MinVal) && (Choice < MS->MaxVal))
          {
               return true;
          }
          return false;
     }

     void ClearChoices(void)
     {
          TotalVal = 0;
          while(pChoiceList->Empty() == false)
               delete pChoiceList->Pop();
     }

     bool GetSelection(C &Var)
     {
          int Choice = RandomLong(0, (TotalVal - 1));
          typename TLinkedList<SMultiChoice*>::node_s *pNode = pChoiceList->GetFirst();

          while(pNode)
          {
               if ((Choice >= pNode->Entry->MinVal) && (Choice < pNode->Entry->MaxVal))
               {
                    Var = pNode->Entry->Choice;
                    return true;
               }
               pNode = pNode->next;
          }

          return false;
     }
};

// ==================================================================
// Code of TMutiChoice - End
// ==================================================================

#endif
