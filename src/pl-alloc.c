/*  pl-alloc.c,v 1.18 1995/02/07 12:12:16 jan Exp

    Copyright (c) 1990 Jan Wielemaker. All rights reserved.
    See ../LICENCE to find out about your rights.
    jan@swi.psy.uva.nl

    Purpose: memory allocation
    MT-status: SAFE
*/

#include "pl-incl.h"

#ifndef ALLOC_DEBUG
#define ALLOC_DEBUG 0
#endif
#define ALLOC_MAGIC 0xbf
#define ALLOC_FREE_MAGIC 0x5f

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This module defines memory allocation for the heap (the  program  space)
and  the  various  stacks.   Memory  allocation below ALLOCFAST bytes is
based entirely on a perfect fit algorithm.  Above ALLOCFAST  the  system
memory  allocation  function  (typically malloc() is used to optimise on
space.  Perfect fit memory allocation is fast and because  most  of  the
memory  is allocated in small segments and these segments normally occur
in similar relative frequencies it does not waste much memory.

The prolog machinery using these memory allocation functions always know
how  much  memory  is  allocated  and  provides  this  argument  to  the
corresponding  unalloc()  call if memory need to be freed.  This saves a
word to store the size of the memory segment.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define LOCK()   PL_LOCK(L_ALLOC)
#define UNLOCK() PL_UNLOCK(L_ALLOC)

typedef struct chunk *	Chunk;
#ifndef ALIGN_SIZE
#if defined(__sgi) && !defined(__GNUC__)
#define ALIGN_SIZE sizeof(double)
#else
#define ALIGN_SIZE sizeof(long)
#endif
#endif
#define ALLOC_MIN  sizeof(Chunk)

struct chunk
{ Chunk		next;		/* next of chain */
};

forwards Chunk	allocate(size_t size);

static char   *spaceptr;	/* alloc: pointer to first free byte */
static size_t spacefree;	/* number of free bytes left */

static Chunk  freeChains[ALLOCFAST/sizeof(Chunk)+1];

#define ALLOCROUND(n) ROUND(n, ALIGN_SIZE)
			   
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Allocate n bytes from the heap.  The amount returned is n rounded up to
a multiple of words.  Allocated memory always starts at a word boundary.

below ALLOCFAST we use a special purpose fast allocation scheme.  Above
(which is very rare) we use Unix malloc()/free() mechanism.

The rest of the code uses the macro allocHeap() to access this function
to avoid problems with 16-bit machines not supporting an ANSI compiler.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

void *
allocHeap(size_t n)
{ Chunk f;
  size_t m;
  
  if ( n == 0 )
    return NULL;

  DEBUG(9, Sdprintf("allocated %ld bytes at ", (unsigned long)n));
  n = ALLOCROUND(n);
  LOCK();
  GD->statistics.heap += n;

  if (n <= ALLOCFAST)
  { m = n / (int) ALIGN_SIZE;
    if ( (f = freeChains[m]) )
    { freeChains[m] = f->next;
      f->next = NULL;
      DEBUG(9, Sdprintf("(r) %p\n", f));
#if ALLOC_DEBUG
      { int i;
	char *s = (char *) f;

	for(i=sizeof(struct chunk); i<n; i++)
	  assert(s[i] == ALLOC_FREE_MAGIC);

	memset(f, ALLOC_MAGIC, n);
      }
#endif
      UNLOCK();
      return f;				/* perfect fit */
    }
    f = allocate(n);			/* allocate from core */

    SetHBase(f);
    SetHTop((char *)f + n);
    UNLOCK();

    DEBUG(9, Sdprintf("(n) %p\n", f));
#if ALLOC_DEBUG
    memset((char *) f, ALLOC_MAGIC, n);
#endif
    return f;
  }

  if ( !(f = malloc(n)) )
    outOfCore();

  SetHBase(f);
  SetHTop((char *)f + n);
  UNLOCK();

  DEBUG(9, Sdprintf("(b) %ld\n", (unsigned long)f));
#if ALLOC_DEBUG
  memset((char *) f, ALLOC_MAGIC, n);
#endif
  return f;
}


void
freeHeap(void *mem, size_t n)
{ Chunk p = (Chunk) mem;

  if ( mem == NULL )
    return;

  n = ALLOCROUND(n);
#if ALLOC_DEBUG
  memset((char *) mem, ALLOC_FREE_MAGIC, n);
#endif
  LOCK();
  GD->statistics.heap -= n;
  DEBUG(9, Sdprintf("freed %ld bytes at %ld\n",
		    (unsigned long)n, (unsigned long)p));

  if (n <= ALLOCFAST)
  { n /= ALIGN_SIZE;
    p->next = freeChains[n];
    freeChains[n] = p;
  } else
  { free(p);
  }
  UNLOCK();
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
No perfect fit is available.  We pick memory from the big chunk  we  are
working  on.   If this is not big enough we will free the remaining part
of it.  Next we check whether any areas are  assigned  to  be  used  for
allocation.   If  all  this fails we allocate new core using Allocate(),
which normally calls malloc(). Early  versions  of  this  module  called
sbrk(),  but  many systems get very upset by using sbrk() in combination
with other memory allocation functions.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static Chunk
allocate(size_t n)
{ char *p;

  if (n <= spacefree)
  { p = spaceptr;
    spaceptr += n;
    spacefree -= n;
    return (Chunk) p;
  }

  if ( spacefree >= sizeof(struct chunk) )
  { int m = spacefree/ALIGN_SIZE;

    if ( m <= (ALLOCFAST/ALIGN_SIZE) )	/* this is freeHeap(), but avoids */
    { Chunk ch = (Chunk)spaceptr;	/* recursive LOCK() */

      ch->next = freeChains[m];
      freeChains[m] = ch;
    }
  }

  if ( !(p = Allocate(ALLOCSIZE)) )
    outOfCore();

  spacefree = ALLOCSIZE;
  spaceptr = p + n;
  spacefree -= n;

  return (Chunk) p;
}

void
initMemAlloc()
{ void *hbase;
  assert(ALIGN_SIZE >= ALLOC_MIN);

  hBase = (char *)(~0L);
  hTop  = (char *)NULL;
  hbase = allocHeap(sizeof(word));
  heap_base = (ulong)hbase & ~0x007fffffL; /* 8MB */
  freeHeap(hbase, sizeof(word));
}

		/********************************
		*             STACKS            *
		*********************************/

void
outOfStack(Stack s, int how)
{ LD->trim_stack_requested = TRUE;

  switch(how)
  { case STACK_OVERFLOW_FATAL:
      LD->outofstack = s;
      warning("Out of %s stack", s->name);

      pl_abort();
      assert(0);
    case STACK_OVERFLOW_SIGNAL_IMMEDIATELY:
      LD->outofstack = NULL;
      gc_status.requested = FALSE;	/* can't have that */
      PL_unify_term(LD->exception.tmp,
		    PL_FUNCTOR, FUNCTOR_error2,
		      PL_FUNCTOR, FUNCTOR_resource_error1,
		        PL_ATOM, ATOM_stack,
		      PL_CHARS, s->name);
      PL_throw(LD->exception.tmp);
      warning("Out of %s stack while not in Prolog!?", s->name);
      assert(0);
    case STACK_OVERFLOW_SIGNAL:
      LD->outofstack = s;
  }
}


volatile void
outOfCore()
{ fatalError("Could not allocate memory: %s", OsError());
}

		 /*******************************
		 *	REFS AND POINTERS	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
__consPtr() is inlined for this module (including pl-wam.c), but external
for the other modules, where it is far less fime-critical.

Actually, for normal operation, consPtr() is a macro from pl-data.h
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#if !defined(consPtr) || defined(SECURE_GC)
#undef consPtr

static inline word
__consPtr(void *p, int ts)
{ unsigned long v = (unsigned long) p;

  v -= base_addresses[ts&STG_MASK];
  assert(v < MAXTAGGEDPTR);
  return (v<<5)|ts;
}

word
consPtr(void *p, int ts)
{ return __consPtr(p, ts);
}

#define consPtr(p, s) __consPtr(p, s)
#endif

#define makeRefL(p) consPtr(p, TAG_REFERENCE|STG_LOCAL)
#define makeRefG(p) consPtr(p, TAG_REFERENCE|STG_GLOBAL)

inline word
__makeRef(Word p)
{ if ( p >= (Word) lBase )
    return makeRefL(p);
  else
    return makeRefG(p);
}


word
makeRef(Word p)
{ return __makeRef(p);			/* public version */
}

#define makeRef(p)  __makeRef(p)


		/********************************
		*        GLOBAL STACK           *
		*********************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
allocGlobal() allocates on the global stack.  Many  functions  do  this
inline  as  it is simple and usualy very time critical.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#if O_SHIFT_STACKS
Word
allocGlobal(int n)
{ Word result;

  if ( roomStack(global)/sizeof(word) < (long) n )
  { growStacks(NULL, NULL, FALSE, TRUE, FALSE);

    if ( roomStack(global)/sizeof(word) < (long) n )
      outOfStack((Stack) &LD->stacks.global, STACK_OVERFLOW_FATAL);
  }

  result = gTop;
  gTop += n;

  return result;
}

#else

inline Word
__allocGlobal(int n)
{ Word result = gTop;

  requireStack(global, n * sizeof(word));
  gTop += n;

  return result;
}

Word allocGlobal(int n)
{ return __allocGlobal(n);
}

#define allocGlobal(n) __allocGlobal(n)

#endif

word
globalFunctor(functor_t f)
{ int arity = arityFunctor(f);
  Word a = allocGlobal(1 + arity);
  Word t = a;

  *a = f;
  while( --arity >= 0 )
    setVar(*++a);

  return consPtr(t, TAG_COMPOUND|STG_GLOBAL);
}


Word
newTerm(void)
{ Word t = allocGlobal(1);

  setVar(*t);

  return t;
}

		 /*******************************
		 *      OPERATIONS ON LONGS	*
		 *******************************/

word
globalLong(long l)
{ Word p = allocGlobal(3);
  word r = consPtr(p, TAG_INTEGER|STG_GLOBAL);
  word m = mkIndHdr(1, TAG_INTEGER);

  *p++ = m;
  *p++ = l;
  *p   = m;
  
  return r;
}


		 /*******************************
		 *    OPERATIONS ON STRINGS	*
		 *******************************/

int
sizeString(word w)
{ word m  = *((Word)addressIndirect(w));
  int wn  = wsizeofInd(m);
  int pad = padHdr(m);

  return wn*sizeof(word) - pad;
}


word
globalNString(long l, const char *s)
{ int lw = (l+sizeof(word))/sizeof(word);
  int pad = (lw*sizeof(word) - l);
  Word p = allocGlobal(2 + lw);
  word r = consPtr(p, TAG_STRING|STG_GLOBAL);
  word m = mkStrHdr(lw, pad);

  *p++ = m;
  p[lw-1] = 0L;				/* write zero's for padding */
  memcpy(p, s, l);
  p += lw;
  *p = m;
  
  return r;
}


word
globalString(const char *s)
{ return globalNString(strlen(s), s);
}



		 /*******************************
		 *     OPERATIONS ON DOUBLES	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Storage of floats (doubles) on the  stacks   and  heap.  Such values are
packed into two `guards words'.  An   intermediate  structure is used to
ensure the possibility of  word-aligned  copy   of  the  data. Structure
assignment is used here  to  avoid  a   loop  for  different  values  of
WORDS_PER_DOUBLE.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define WORDS_PER_DOUBLE ((sizeof(double)+sizeof(word)-1)/sizeof(word))

typedef struct
{ word w[WORDS_PER_DOUBLE];
} fword;


void
doublecpy(void *to, void *from)
{ fword *t = to;
  fword *f = from;

  *t = *f;
}


double					/* take care of alignment! */
valReal(word w)
{ fword *v = (fword *)valIndirectP(w);
  union
  { double d;
    fword  l;
  } val;
  
  val.l = *v;

  return val.d;
}


word
globalReal(double d)
{ Word p = allocGlobal(2+WORDS_PER_DOUBLE);
  word r = consPtr(p, TAG_FLOAT|STG_GLOBAL);
  word m = mkIndHdr(WORDS_PER_DOUBLE, TAG_FLOAT);
  union
  { double d;
    fword  l;
  } val;
  fword *v;

  val.d = d;
  *p++ = m;
  v = (fword *)p;
  *v++ = val.l;
  p = (Word) v;
  *p   = m;

  return r;
}


		 /*******************************
		 *  GENERIC INDIRECT OPERATIONS	*
		 *******************************/

int
equalIndirect(word w1, word w2)
{ Word p1 = addressIndirect(w1);
  Word p2 = addressIndirect(w2);
  
  if ( *p1 == *p2 )
  { int n = wsizeofInd(*p1);
    
    while( --n >= 0 )
    { if ( *++p1 != *++p2 )
	fail;
    }

    succeed;
  }

  fail;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Copy an indirect data object to the heap.  The type is not of importance,
neither is the length or original location.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

word
globalIndirect(word w)
{ Word p = addressIndirect(w);
  word t = *p;
  int  n = wsizeofInd(t);
  Word h = allocGlobal((n+2));
  Word hp = h;
  
  *hp = t;
  while(--n >= 0)
    *++hp = *++p;
  *++hp = t;

  return consPtr(h, tag(w)|STG_GLOBAL);
}


word
globalIndirectFromCode(Code *PC)
{ Code pc = *PC;
  word m = *pc++;
  int  n = wsizeofInd(m);
  Word p = allocGlobal(n+2);
  word r = consPtr(p, tag(m)|STG_GLOBAL);

  *p++ = m;
  while(--n >= 0)
    *p++ = *pc++;
  *p++ = m;

  *PC = pc;
  return r;
}


static int				/* used in pl-wam.c */
equalIndirectFromCode(word a, Code *PC)
{ Word pc = *PC;
  Word pa = addressIndirect(a);

  if ( *pc == *pa )
  { int  n = wsizeofInd(*pc);

    while(--n >= 0)
    { if ( *++pc != *++pa )
	fail;
    }
    pc++;
    *PC = pc;
    succeed;
  }

  fail;
}


		/********************************
		*            STRINGS            *
		*********************************/

char *
store_string(const char *s)
{ char *copy = (char *)allocHeap(strlen(s)+1);

  strcpy(copy, s);
  return copy;
}


void
remove_string(char *s)
{ if ( s )
    freeHeap(s, strlen(s)+1);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Hash function for strings.  This function has been evaluated on Shelley,
which defines about 5000 Prolog atoms.  It produces a very nice  uniform
distribution over these atoms.  Note that size equals 2^n.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
unboundStringHashValue(const char *t)
{ unsigned int value = 0;
  unsigned int shift = 5;

  while(*t)
  { unsigned int c = *t++;
    
    c -= 'a';
    value ^= c << (shift & 0xf);
    shift ^= c;
  }

  return value ^ (value >> 16);
}


		 /*******************************
		 *	     GNU MALLOC		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
These functions are used by various GNU-libraries and -when not linked
with the GNU C-library lead to undefined symbols.  Therefore we define
them in SWI-Prolog so that we can also give consistent warnings.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

void *
xmalloc(size_t size)
{ void *mem;

  if ( (mem = malloc(size)) )
    return mem;
  if ( size )
    outOfCore();

  return NULL;
}


void *
xrealloc(void *mem, size_t size)
{ void *newmem;

  newmem = mem ? realloc(mem, size) : malloc(size);
  if ( newmem )
    return newmem;
  if ( size )
    outOfCore();

  return NULL;
}

#undef LOCK
#undef UNLOCK
