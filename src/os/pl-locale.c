/*  Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        J.Wielemaker@vu.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 2013, VU University Amsterdam

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "pl-incl.h"
#include "pl-locale.h"

#ifdef O_LOCALE

#include <locale.h>

#define LOCK()   PL_LOCK(L_LOCALE)	/* MT locking */
#define UNLOCK() PL_UNLOCK(L_LOCALE)

#undef LD				/* fetch LD once per function */
#define LD LOCAL_LD

#define LSTR_MAX 16

static wchar_t *
ls_to_wcs(const char *in, const wchar_t *on_error)
{ wchar_t buf[LSTR_MAX];
  mbstate_t state;

  memset(&state, 0, sizeof(state));
  mbsrtowcs(buf, &in, LSTR_MAX, &state);
  if ( in == NULL )
  { return wcsdup(buf);
  } else
  { Sdprintf("Illegal locale string: %s\n", in);
    return wcsdup(on_error);
  }
}


static int
init_locale_strings(PL_locale *l, struct lconv *conv)
{ if ( conv )
  { l->decimal_point = ls_to_wcs(conv->decimal_point, L".");
    l->thousands_sep = ls_to_wcs(conv->thousands_sep, L",");
    l->grouping      = strdup(conv->grouping);

    return TRUE;
  } else
  { l->decimal_point = wcsdup(L".");
    l->thousands_sep = wcsdup(L",");
    l->grouping      = strdup("\003");

    return FALSE;
  }
}


static PL_locale *
new_locale(PL_locale *proto)
{ PL_locale *new = PL_malloc(sizeof(*new));

  if ( new )
  { memset(new, 0, sizeof(*new));
    new->magic = LOCALE_MAGIC;

    if ( proto )
    { new->decimal_point = wcsdup(proto->decimal_point);
      new->thousands_sep = wcsdup(proto->thousands_sep);
      new->grouping      = strdup(proto->grouping);
    } else
    { init_locale_strings(new, localeconv());
    }
  }

  return new;
}


static void
free_locale_strings(PL_locale *l)
{ free(l->decimal_point);
  free(l->thousands_sep);
  free(l->grouping);
}

static void
free_locale(PL_locale *l)
{ if ( l )
  { free_locale_strings(l);

    if ( l->alias )
      PL_unregister_atom(l->alias);

    PL_free(l);
  }
}

static void
update_locale(PL_locale *l, int category, const char *locale)
{ free_locale_strings(l);
  init_locale_strings(l, localeconv());
}


static int
alias_locale(PL_locale *l, atom_t alias)
{ int rc;

  LOCK();

  if ( !GD->locale.localeTable )
    GD->locale.localeTable = newHTable(16);

  if ( addHTable(GD->locale.localeTable, (void*)alias, l) )
  { l->alias = alias;
    PL_register_atom(alias);
    rc = TRUE;
  } else
  { GET_LD
    term_t obj = PL_new_term_ref();

    PL_put_atom(obj, alias);
    rc = PL_error("locale_create", 2, "Alias name already taken",
		  ERR_PERMISSION, ATOM_create, ATOM_locale, obj);
  }
  UNLOCK();

  return rc;
}


		 /*******************************
		 *	  LOCALE BLOB		*
		 *******************************/

typedef struct locale_ref
{ PL_locale	*data;
} locale_ref;


static int
write_locale_ref(IOSTREAM *s, atom_t aref, int flags)
{ locale_ref *ref = PL_blob_data(aref, NULL, NULL);
  (void)flags;

  Sfprintf(s, "<locale>(%p)", ref->data);

  return TRUE;
}


static void
acquire_locale_ref(atom_t aref)
{ locale_ref *ref = PL_blob_data(aref, NULL, NULL);

  (void)ref;
}


static int
release_locale_ref(atom_t aref)
{ locale_ref *ref = PL_blob_data(aref, NULL, NULL);

  LOCK();
  if ( ref->data->references == 0 )
    free_locale(ref->data);
  else
    ref->data->symbol = 0;
  UNLOCK();

  return TRUE;
}


static int
save_locale_ref(atom_t aref, IOSTREAM *fd)
{ locale_ref *ref = PL_blob_data(aref, NULL, NULL);
  (void)fd;

  return PL_warning("Cannot save reference to <locale>(%p)", ref->data);
}


static atom_t
load_locale_ref(IOSTREAM *fd)
{ (void)fd;

  return PL_new_atom("<saved-locale-ref>");
}


static PL_blob_t locale_blob =
{ PL_BLOB_MAGIC,
  PL_BLOB_UNIQUE,
  "locale",
  release_locale_ref,
  NULL,
  write_locale_ref,
  acquire_locale_ref,
  save_locale_ref,
  load_locale_ref
};


		 /*******************************
		 *	   PROLOG HANDLE	*
		 *******************************/

int
unifyLocale(term_t t, PL_locale *l)
{ GET_LD
  term_t b;

  if ( l->alias )
    return PL_unify_atom(t, l->alias);

  if ( l->symbol )
    return PL_unify_atom(t, l->symbol);

  if ( (b=PL_new_term_ref()) &&
       PL_put_blob(b, &l, sizeof(l), &locale_blob) )
  { PL_get_atom(b, &l->symbol);
    assert(l->symbol);
    return PL_unify(t, b);
  }

  return FALSE;
}


int
getLocale(term_t t, PL_locale **lp)
{ GET_LD
  atom_t a;

  if ( PL_get_atom(t, &a) )
  { PL_locale *l = NULL;
    PL_blob_t *bt;
    locale_ref *ref;

    if ( a == ATOM_current )
    { GET_LD

      l = LD->locale.current;
    } else if ( (ref=PL_blob_data(a, NULL, &bt)) && bt == &locale_blob )
    { l = ref->data;
    } else if ( GD->locale.localeTable )
    { Symbol s;

      if ( (s=lookupHTable(GD->locale.localeTable, (void*)a)) )
	l = s->value;
    }

    if ( l )
    { assert(l->magic == LOCALE_MAGIC);
      *lp = l;
      return TRUE;
    }
  }

  return FALSE;
}


int
getLocaleEx(term_t t, PL_locale **lp)
{ GET_LD

  if ( getLocale(t, lp) )
    return TRUE;

  if ( PL_is_atom(t) )
    return PL_existence_error("locale", t);
  else
    return PL_type_error("locale", t);
}


		 /*******************************
		 *	 PROLOG BINDING		*
		 *******************************/

static int		/* locale_property(Mutex, alias(Name)) */
locale_alias_property(PL_locale *l, term_t prop ARG_LD)
{ if ( l->alias )
    return PL_unify_atom(prop, l->alias);

  return FALSE;
}

static int		/* locale_property(Locale, decimal_point(Atom)) */
locale_decimal_point_property(PL_locale *l, term_t prop ARG_LD)
{ if ( l->decimal_point && l->decimal_point[0] )
    return PL_unify_wchars(prop, PL_ATOM, (size_t)-1, l->decimal_point);

  return FALSE;
}

static int		/* locale_property(Locale, thousands_sep(Atom)) */
locale_thousands_sep_property(PL_locale *l, term_t prop ARG_LD)
{ if ( l->thousands_sep && l->thousands_sep[0] )
    return PL_unify_wchars(prop, PL_ATOM, (size_t)-1, l->thousands_sep);

  return FALSE;
}

static int		/* locale_property(Locale, grouping(List)) */
locale_grouping_property(PL_locale *l, term_t prop ARG_LD)
{ if ( l->grouping && l->grouping[0] )
  { term_t tail = PL_copy_term_ref(prop);
    term_t head = PL_new_term_ref();
    char *s;

    for(s=l->grouping; ; s++)
    { if ( !PL_unify_list(tail, head, tail) )
	return FALSE;
      if ( s[1] == 0 || (s[1] == s[0] && s[2] == 0) )
	return ( PL_unify_term(head, PL_FUNCTOR, FUNCTOR_repeat1,
			       PL_INT, (int)s[0]) &&
		 PL_unify_nil(tail)
	       );
      if ( s[0] == CHAR_MAX )
	return PL_unify_nil(tail);
      if ( !PL_unify_integer(head, s[0]) )
	return FALSE;
    }
  }

  return FALSE;
}


typedef struct
{ functor_t functor;			/* functor of property */
  int (*function)();			/* function to generate */
} lprop;

static const lprop lprop_list [] =
{ { FUNCTOR_alias1,	    locale_alias_property },
  { FUNCTOR_decimal_point1, locale_decimal_point_property },
  { FUNCTOR_thousands_sep1, locale_thousands_sep_property },
  { FUNCTOR_grouping1,      locale_grouping_property },
  { 0,			    NULL }
};

typedef struct
{ TableEnum e;				/* Enumerator on mutex-table */
  PL_locale *l;				/* current locale */
  const lprop *p;			/* Pointer in properties */
  int enum_properties;			/* Enumerate the properties */
} lprop_enum;


static int
get_prop_def(term_t t, atom_t expected, const lprop *list, const lprop **def)
{ GET_LD
  functor_t f;

  if ( PL_get_functor(t, &f) )
  { const lprop *p = list;

    for( ; p->functor; p++ )
    { if ( f == p->functor )
      { *def = p;
        return TRUE;
      }
    }

    PL_error(NULL, 0, NULL, ERR_DOMAIN, expected, t);
    return -1;
  }

  if ( PL_is_variable(t) )
    return 0;

  PL_error(NULL, 0, NULL, ERR_TYPE, expected, t);
  return -1;
}


static int
advance_lstate(lprop_enum *state)
{ if ( state->enum_properties )
  { state->p++;
    if ( state->p->functor )
      return TRUE;

    state->p = lprop_list;
  }
  if ( state->e )
  { Symbol s;

    if ( (s = advanceTableEnum(state->e)) )
    { state->l = s->value;

      return TRUE;
    }
  }

  return FALSE;
}


static void
free_lstate(lprop_enum *state)
{ if ( state->e )
    freeTableEnum(state->e);

  freeForeignState(state, sizeof(*state));
}


/** locale_property(?Locale, ?Property) is nondet.
*/

static
PRED_IMPL("locale_property", 2, locale_property, PL_FA_NONDETERMINISTIC)
{ PRED_LD
  term_t locale = A1;
  term_t property = A2;
  lprop_enum statebuf;
  lprop_enum *state;

  switch( CTX_CNTRL )
  { case FRG_FIRST_CALL:
    { memset(&statebuf, 0, sizeof(statebuf));
      state = &statebuf;

      if ( PL_is_variable(locale) )
      { switch( get_prop_def(property, ATOM_locale_property,
			     lprop_list, &state->p) )
	{ case 1:
	    state->e = newTableEnum(GD->locale.localeTable);
	    goto enumerate;
	  case 0:
	    state->e = newTableEnum(GD->locale.localeTable);
	    state->p = lprop_list;
	    state->enum_properties = TRUE;
	    goto enumerate;
	  case -1:
	    return FALSE;
	}
      } else if ( getLocale(locale, &state->l) )
      { switch( get_prop_def(property, ATOM_locale_property,
			     lprop_list, &state->p) )
	{ case 1:
	    goto enumerate;
	  case 0:
	    state->p = lprop_list;
	    state->enum_properties = TRUE;
	    goto enumerate;
	  case -1:
	    return FALSE;
	}
      } else
      { return FALSE;
      }
    }
    case FRG_REDO:
      state = CTX_PTR;
      break;
    case FRG_CUTTED:
      state = CTX_PTR;
      free_lstate(state);
      succeed;
    default:
      assert(0);
  }

enumerate:
  if ( !state->l )			/* first time, enumerating locales */
  { Symbol s;

    assert(state->e);
    if ( (s=advanceTableEnum(state->e)) )
    { state->l = s->value;
    } else
    { freeTableEnum(state->e);
      assert(state != &statebuf);
      return FALSE;
    }
  }

  { term_t arg = PL_new_term_ref();

    if ( !state->enum_properties )
      _PL_get_arg(1, property, arg);

    for(;;)
    { if ( (*state->p->function)(state->l, arg PASS_LD) )
      { if ( state->enum_properties )
	{ if ( !PL_unify_term(property,
			      PL_FUNCTOR, state->p->functor,
			        PL_TERM, arg) )
	    goto error;
	}
	if ( state->e )
	{ if ( !unifyLocale(locale, state->l) )
	    goto error;
	}

	if ( advance_lstate(state) )
	{ if ( state == &statebuf )
	  { lprop_enum *copy = allocForeignState(sizeof(*copy));

	    *copy = *state;
	    state = copy;
	  }

	  ForeignRedoPtr(state);
	}

	if ( state != &statebuf )
	  free_lstate(state);
	return TRUE;
      }

      if ( !advance_lstate(state) )
      { error:
	if ( state != &statebuf )
	  free_lstate(state);
	return FALSE;
      }
    }
  }
}


static int
set_chars(term_t t, wchar_t **valp)
{ wchar_t *s;

  if ( PL_get_wchars(t, NULL, &s, CVT_ATOM|CVT_EXCEPTION) )
  { free(*valp);
    if ( (*valp = wcsdup(s)) )
      return TRUE;
    return PL_no_memory();
  }

  return FALSE;
}


#define MAX_GROUPING 10

static int
get_group_size_ex(term_t t, int *s)
{ int i;

  if ( PL_get_integer_ex(t, &i) )
  { if ( i > 0 && i < CHAR_MAX )
    { *s = i;
      return TRUE;
    }
    return PL_domain_error("digit_group_size", t);
  }

  return FALSE;
}


static int
set_grouping(term_t t, char **valp)
{ GET_LD
  char s[MAX_GROUPING];
  term_t tail = PL_copy_term_ref(t);
  term_t head = PL_new_term_ref();
  char *o = s;

  while(PL_get_list_ex(tail, head, tail))
  { int g;

    if ( o-s+2 >= MAX_GROUPING )
      return PL_representation_error("digit_groups");

    if ( PL_is_functor(head, FUNCTOR_repeat1) )
    { _PL_get_arg(1, head, head);
      if ( get_group_size_ex(head, &g) )
      { *o++ = g;
	*o++ = CHAR_MAX;
	break;					/* must be last in list */
      }
      return FALSE;
    }
    if ( get_group_size_ex(head, &g) )
    { *o++ = g;
    } else
      return FALSE;
  }
  if ( PL_get_nil_ex(tail) )
  { *o++ = '\0';
    free(*valp);
    if ( (*valp = strdup(s)) )
      return TRUE;
    return PL_no_memory();
  }

  return FALSE;
}


/** locale_create(-Locale, +Default, +Options) is det.
*/

static
PRED_IMPL("locale_create", 3, locale_create, 0)
{ PRED_LD
  PL_locale *def, *new;

  if ( !getLocaleEx(A2, &def) )
    return FALSE;
  if ( (new=new_locale(def)) )
  { atom_t alias = 0;
    term_t tail = PL_copy_term_ref(A3);
    term_t head = PL_new_term_ref();
    term_t arg  = PL_new_term_ref();

    while(PL_get_list_ex(tail, head, tail))
    { atom_t pname;
      int parity;

      if ( !PL_get_name_arity(head, &pname, &parity) ||
	   parity != 1 ||
	   !PL_get_arg(1, head, arg) )
      { PL_type_error("locale_property", head);
	goto error;
      }
      if ( pname == ATOM_alias )
      { if ( !PL_get_atom_ex(arg, &alias) )
	  goto error;
      } else if ( pname == ATOM_decimal_point )
      { if ( !set_chars(arg, &new->decimal_point) )
	  goto error;
      } else if ( pname == ATOM_thousands_sep )
      { if ( !set_chars(arg, &new->thousands_sep) )
	  goto error;
      } else if ( pname == ATOM_grouping )
      { if ( !set_grouping(arg, &new->grouping) )
	  goto error;
      }
    }
    if ( !PL_get_nil_ex(tail) )
    {
    error:
      free_locale(new);
      return FALSE;
    }

    if ( alias && !alias_locale(new, alias) )
      goto error;

    return unifyLocale(A1, new);
  } else
  { return PL_no_memory();
  }
}


		 /*******************************
		 *	     C INTERFACE	*
		 *******************************/


static void
initDefaultsStreamsLocale(PL_locale *l)
{ IOSTREAM *s = S__getiob();
  int i;

  for(i=0; i<2; i++, s++)
  { if ( !s->locale )
      s->locale = acquireLocale(l);
  }
}


void
initLocale(void)
{ GET_LD
  PL_locale *def;

  if ( !setlocale(LC_NUMERIC, "") )
  { DEBUG(0, Sdprintf("Failed to set LC_NUMERIC locale\n"));
  }

  if ( (def = new_locale(NULL)) )
  { alias_locale(def, ATOM_default);
    def->references++;
    GD->locale.default_locale = def;
    LD->locale.current = acquireLocale(def);

    initDefaultsStreamsLocale(def);
  }
}


void
updateLocale(int category, const char *locale)
{ update_locale(GD->locale.default_locale, category, locale);
}


int
initStreamLocale(IOSTREAM *s)
{ GET_LD
  PL_locale *l;

  if ( LD )					/* a Prolog thread */
    l = LD->locale.current;
  else
    l = GD->locale.default_locale;

  if ( l )
    s->locale = acquireLocale(l);

  return TRUE;
}


PL_locale *
acquireLocale(PL_locale *l)
{ LOCK();
  l->references++;
  UNLOCK();

  return l;
}


void
releaseLocale(PL_locale *l)
{ LOCK();
  if ( --l->references == 0 && l->symbol == 0 && l->alias == 0 )
    free_locale(l);
  UNLOCK();
}




		 /*******************************
		 *      PUBLISH PREDICATES	*
		 *******************************/

BeginPredDefs(locale)
  PRED_DEF("locale_property", 2, locale_property, PL_FA_NONDETERMINISTIC)
  PRED_DEF("locale_create",   3, locale_create,   0)
EndPredDefs

#endif /*O_LOCALE*/
