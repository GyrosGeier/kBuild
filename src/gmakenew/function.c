/* Builtin function expansion for GNU Make.
Copyright (C) 1988, 1989, 1990, 1991, 1992, 1993, 1994, 1995, 1996, 1997,
1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005, 2006 Free Software
Foundation, Inc.
This file is part of GNU Make.

GNU Make is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 2, or (at your option) any later version.

GNU Make is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
GNU Make; see the file COPYING.  If not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.  */

#ifdef CONFIG_WITH_OPTIMIZATION_HACKS
# include <assert.h>
#endif
#include "make.h"
#include "filedef.h"
#include "variable.h"
#include "dep.h"
#include "job.h"
#include "commands.h"
#include "debug.h"

#ifdef _AMIGA
#include "amiga.h"
#endif

#ifdef WINDOWS32 /* bird */
# include "pathstuff.h"
#endif

#ifdef KMK_HELPERS
# include "kbuild.h"
#endif
#ifdef CONFIG_WITH_XARGS /* bird */
# ifdef HAVE_LIMITS_H
#  include <limits.h>
# endif
#endif


struct function_table_entry
  {
    const char *name;
    unsigned char len;
    unsigned char minimum_args;
    unsigned char maximum_args;
    char expand_args;
    char *(*func_ptr) (char *output, char **argv, const char *fname);
  };

static unsigned long
function_table_entry_hash_1 (const void *keyv)
{
  const struct function_table_entry *key = keyv;
  return_STRING_N_HASH_1 (key->name, key->len);
}

static unsigned long
function_table_entry_hash_2 (const void *keyv)
{
  const struct function_table_entry *key = keyv;
  return_STRING_N_HASH_2 (key->name, key->len);
}

static int
function_table_entry_hash_cmp (const void *xv, const void *yv)
{
  const struct function_table_entry *x = xv;
  const struct function_table_entry *y = yv;
  int result = x->len - y->len;
  if (result)
    return result;
  return_STRING_N_COMPARE (x->name, y->name, x->len);
}

static struct hash_table function_table;


/* Store into VARIABLE_BUFFER at O the result of scanning TEXT and replacing
   each occurrence of SUBST with REPLACE. TEXT is null-terminated.  SLEN is
   the length of SUBST and RLEN is the length of REPLACE.  If BY_WORD is
   nonzero, substitutions are done only on matches which are complete
   whitespace-delimited words.  */

char *
subst_expand (char *o, const char *text, const char *subst, const char *replace,
              unsigned int slen, unsigned int rlen, int by_word)
{
  const char *t = text;
  const char *p;

  if (slen == 0 && !by_word)
    {
      /* The first occurrence of "" in any string is its end.  */
      o = variable_buffer_output (o, t, strlen (t));
      if (rlen > 0)
	o = variable_buffer_output (o, replace, rlen);
      return o;
    }

  do
    {
      if (by_word && slen == 0)
	/* When matching by words, the empty string should match
	   the end of each word, rather than the end of the whole text.  */
	p = end_of_token (next_token (t));
      else
	{
	  p = strstr (t, subst);
	  if (p == 0)
	    {
	      /* No more matches.  Output everything left on the end.  */
	      o = variable_buffer_output (o, t, strlen (t));
	      return o;
	    }
	}

      /* Output everything before this occurrence of the string to replace.  */
      if (p > t)
	o = variable_buffer_output (o, t, p - t);

      /* If we're substituting only by fully matched words,
	 or only at the ends of words, check that this case qualifies.  */
      if (by_word
          && ((p > text && !isblank ((unsigned char)p[-1]))
              || (p[slen] != '\0' && !isblank ((unsigned char)p[slen]))))
	/* Struck out.  Output the rest of the string that is
	   no longer to be replaced.  */
	o = variable_buffer_output (o, subst, slen);
      else if (rlen > 0)
	/* Output the replacement string.  */
	o = variable_buffer_output (o, replace, rlen);

      /* Advance T past the string to be replaced.  */
      t = p + slen;
    } while (*t != '\0');

  return o;
}


/* Store into VARIABLE_BUFFER at O the result of scanning TEXT
   and replacing strings matching PATTERN with REPLACE.
   If PATTERN_PERCENT is not nil, PATTERN has already been
   run through find_percent, and PATTERN_PERCENT is the result.
   If REPLACE_PERCENT is not nil, REPLACE has already been
   run through find_percent, and REPLACE_PERCENT is the result.
   Note that we expect PATTERN_PERCENT and REPLACE_PERCENT to point to the
   character _AFTER_ the %, not to the % itself.
*/

char *
patsubst_expand_pat (char *o, const char *text,
                     const char *pattern, const char *replace,
                     const char *pattern_percent, const char *replace_percent)
{
  unsigned int pattern_prepercent_len, pattern_postpercent_len;
  unsigned int replace_prepercent_len, replace_postpercent_len;
  const char *t;
  unsigned int len;
  int doneany = 0;

  /* Record the length of REPLACE before and after the % so we don't have to
     compute these lengths more than once.  */
  if (replace_percent)
    {
      replace_prepercent_len = replace_percent - replace - 1;
      replace_postpercent_len = strlen (replace_percent);
    }
  else
    {
      replace_prepercent_len = strlen (replace);
      replace_postpercent_len = 0;
    }

  if (!pattern_percent)
    /* With no % in the pattern, this is just a simple substitution.  */
    return subst_expand (o, text, pattern, replace,
			 strlen (pattern), strlen (replace), 1);

  /* Record the length of PATTERN before and after the %
     so we don't have to compute it more than once.  */
  pattern_prepercent_len = pattern_percent - pattern - 1;
  pattern_postpercent_len = strlen (pattern_percent);

  while ((t = find_next_token (&text, &len)) != 0)
    {
      int fail = 0;

      /* Is it big enough to match?  */
      if (len < pattern_prepercent_len + pattern_postpercent_len)
	fail = 1;

      /* Does the prefix match? */
      if (!fail && pattern_prepercent_len > 0
	  && (*t != *pattern
	      || t[pattern_prepercent_len - 1] != pattern_percent[-2]
	      || !strneq (t + 1, pattern + 1, pattern_prepercent_len - 1)))
	fail = 1;

      /* Does the suffix match? */
      if (!fail && pattern_postpercent_len > 0
	  && (t[len - 1] != pattern_percent[pattern_postpercent_len - 1]
	      || t[len - pattern_postpercent_len] != *pattern_percent
	      || !strneq (&t[len - pattern_postpercent_len],
			  pattern_percent, pattern_postpercent_len - 1)))
	fail = 1;

      if (fail)
	/* It didn't match.  Output the string.  */
	o = variable_buffer_output (o, t, len);
      else
	{
	  /* It matched.  Output the replacement.  */

	  /* Output the part of the replacement before the %.  */
	  o = variable_buffer_output (o, replace, replace_prepercent_len);

	  if (replace_percent != 0)
	    {
	      /* Output the part of the matched string that
		 matched the % in the pattern.  */
	      o = variable_buffer_output (o, t + pattern_prepercent_len,
					  len - (pattern_prepercent_len
						 + pattern_postpercent_len));
	      /* Output the part of the replacement after the %.  */
	      o = variable_buffer_output (o, replace_percent,
					  replace_postpercent_len);
	    }
	}

      /* Output a space, but not if the replacement is "".  */
      if (fail || replace_prepercent_len > 0
	  || (replace_percent != 0 && len + replace_postpercent_len > 0))
	{
	  o = variable_buffer_output (o, " ", 1);
	  doneany = 1;
	}
    }
  if (doneany)
    /* Kill the last space.  */
    --o;

  return o;
}

/* Store into VARIABLE_BUFFER at O the result of scanning TEXT
   and replacing strings matching PATTERN with REPLACE.
   If PATTERN_PERCENT is not nil, PATTERN has already been
   run through find_percent, and PATTERN_PERCENT is the result.
   If REPLACE_PERCENT is not nil, REPLACE has already been
   run through find_percent, and REPLACE_PERCENT is the result.
   Note that we expect PATTERN_PERCENT and REPLACE_PERCENT to point to the
   character _AFTER_ the %, not to the % itself.
*/

char *
patsubst_expand (char *o, const char *text, char *pattern, char *replace)
{
  const char *pattern_percent = find_percent (pattern);
  const char *replace_percent = find_percent (replace);

  /* If there's a percent in the pattern or replacement skip it.  */
  if (replace_percent)
    ++replace_percent;
  if (pattern_percent)
    ++pattern_percent;

  return patsubst_expand_pat (o, text, pattern, replace,
                              pattern_percent, replace_percent);
}

#ifdef CONFIG_WITH_OPTIMIZATION_HACKS
/* The maximum length of a function, once reached there is
   it can't be function and we can skip the hash lookup drop out. */

# ifdef KMK
#  define MAX_FUNCTION_LENGTH 12
# else
#  define MAX_FUNCTION_LENGTH 10
# endif
#endif /* CONFIG_WITH_OPTIMIZATION_HACKS */

/* Look up a function by name.  */

#ifdef CONFIG_WITH_OPTIMIZATION_HACKS
__inline
#endif /* CONFIG_WITH_OPTIMIZATION_HACKS */
static const struct function_table_entry *
lookup_function (const char *s)
{
  const char *e = s;
#ifdef CONFIG_WITH_OPTIMIZATION_HACKS
  int left = MAX_FUNCTION_LENGTH;
  int ch;
  while (((ch = *e) >= 'a' && ch <='z') || ch == '-')
    {
      if (!left--)
        return 0;
      e++;
    }
#else
  while (*e && ( (*e >= 'a' && *e <= 'z') || *e == '-'))
    e++;
#endif
  if (*e == '\0' || isblank ((unsigned char) *e))
    {
      struct function_table_entry function_table_entry_key;
      function_table_entry_key.name = s;
      function_table_entry_key.len = e - s;

      return hash_find_item (&function_table, &function_table_entry_key);
    }
  return 0;
}


/* Return 1 if PATTERN matches STR, 0 if not.  */

int
pattern_matches (const char *pattern, const char *percent, const char *str)
{
  unsigned int sfxlen, strlength;

  if (percent == 0)
    {
      unsigned int len = strlen (pattern) + 1;
      char *new_chars = alloca (len);
      memcpy (new_chars, pattern, len);
      percent = find_percent (new_chars);
      if (percent == 0)
	return streq (new_chars, str);
      pattern = new_chars;
    }

  sfxlen = strlen (percent + 1);
  strlength = strlen (str);

  if (strlength < (percent - pattern) + sfxlen
      || !strneq (pattern, str, percent - pattern))
    return 0;

  return !strcmp (percent + 1, str + (strlength - sfxlen));
}


/* Find the next comma or ENDPAREN (counting nested STARTPAREN and
   ENDPARENtheses), starting at PTR before END.  Return a pointer to
   next character.

   If no next argument is found, return NULL.
*/

static char *
find_next_argument (char startparen, char endparen,
                    const char *ptr, const char *end)
{
  int count = 0;

  for (; ptr < end; ++ptr)
    if (*ptr == startparen)
      ++count;

    else if (*ptr == endparen)
      {
	--count;
	if (count < 0)
	  return NULL;
      }

    else if (*ptr == ',' && !count)
      return (char *)ptr;

  /* We didn't find anything.  */
  return NULL;
}


/* Glob-expand LINE.  The returned pointer is
   only good until the next call to string_glob.  */

static char *
string_glob (char *line)
{
  static char *result = 0;
  static unsigned int length;
  struct nameseq *chain;
  unsigned int idx;

  chain = multi_glob (parse_file_seq
		      (&line, '\0', sizeof (struct nameseq),
		       /* We do not want parse_file_seq to strip `./'s.
			  That would break examples like:
			  $(patsubst ./%.c,obj/%.o,$(wildcard ./?*.c)).  */
		       0),
		      sizeof (struct nameseq));

  if (result == 0)
    {
      length = 100;
      result = xmalloc (100);
    }

  idx = 0;
  while (chain != 0)
    {
      const char *name = chain->name;
      unsigned int len = strlen (name);

      struct nameseq *next = chain->next;
      free (chain);
      chain = next;

      /* multi_glob will pass names without globbing metacharacters
	 through as is, but we want only files that actually exist.  */
      if (file_exists_p (name))
	{
	  if (idx + len + 1 > length)
	    {
	      length += (len + 1) * 2;
	      result = xrealloc (result, length);
	    }
	  memcpy (&result[idx], name, len);
	  idx += len;
	  result[idx++] = ' ';
	}
    }

  /* Kill the last space and terminate the string.  */
  if (idx == 0)
    result[0] = '\0';
  else
    result[idx - 1] = '\0';

  return result;
}

/*
  Builtin functions
 */

static char *
func_patsubst (char *o, char **argv, const char *funcname UNUSED)
{
  o = patsubst_expand (o, argv[2], argv[0], argv[1]);
  return o;
}


static char *
func_join (char *o, char **argv, const char *funcname UNUSED)
{
  int doneany = 0;

  /* Write each word of the first argument directly followed
     by the corresponding word of the second argument.
     If the two arguments have a different number of words,
     the excess words are just output separated by blanks.  */
  const char *tp;
  const char *pp;
  const char *list1_iterator = argv[0];
  const char *list2_iterator = argv[1];
  do
    {
      unsigned int len1, len2;

      tp = find_next_token (&list1_iterator, &len1);
      if (tp != 0)
	o = variable_buffer_output (o, tp, len1);

      pp = find_next_token (&list2_iterator, &len2);
      if (pp != 0)
	o = variable_buffer_output (o, pp, len2);

      if (tp != 0 || pp != 0)
	{
	  o = variable_buffer_output (o, " ", 1);
	  doneany = 1;
	}
    }
  while (tp != 0 || pp != 0);
  if (doneany)
    /* Kill the last blank.  */
    --o;

  return o;
}


static char *
func_origin (char *o, char **argv, const char *funcname UNUSED)
{
  /* Expand the argument.  */
  struct variable *v = lookup_variable (argv[0], strlen (argv[0]));
  if (v == 0)
    o = variable_buffer_output (o, "undefined", 9);
  else
    switch (v->origin)
      {
      default:
      case o_invalid:
	abort ();
	break;
      case o_default:
	o = variable_buffer_output (o, "default", 7);
	break;
      case o_env:
	o = variable_buffer_output (o, "environment", 11);
	break;
      case o_file:
	o = variable_buffer_output (o, "file", 4);
	break;
      case o_env_override:
	o = variable_buffer_output (o, "environment override", 20);
	break;
      case o_command:
	o = variable_buffer_output (o, "command line", 12);
	break;
      case o_override:
	o = variable_buffer_output (o, "override", 8);
	break;
      case o_automatic:
	o = variable_buffer_output (o, "automatic", 9);
	break;
      }

  return o;
}

static char *
func_flavor (char *o, char **argv, const char *funcname UNUSED)
{
  struct variable *v = lookup_variable (argv[0], strlen (argv[0]));

  if (v == 0)
    o = variable_buffer_output (o, "undefined", 9);
  else
    if (v->recursive)
      o = variable_buffer_output (o, "recursive", 9);
    else
      o = variable_buffer_output (o, "simple", 6);

  return o;
}

#ifdef VMS
# define IS_PATHSEP(c) ((c) == ']')
#else
# ifdef HAVE_DOS_PATHS
#  define IS_PATHSEP(c) ((c) == '/' || (c) == '\\')
# else
#  define IS_PATHSEP(c) ((c) == '/')
# endif
#endif


static char *
func_notdir_suffix (char *o, char **argv, const char *funcname)
{
  /* Expand the argument.  */
  const char *list_iterator = argv[0];
  const char *p2;
  int doneany =0;
  unsigned int len=0;

  int is_suffix = streq (funcname, "suffix");
  int is_notdir = !is_suffix;
  while ((p2 = find_next_token (&list_iterator, &len)) != 0)
    {
      const char *p = p2 + len;


      while (p >= p2 && (!is_suffix || *p != '.'))
	{
	  if (IS_PATHSEP (*p))
	    break;
	  --p;
	}

      if (p >= p2)
	{
	  if (is_notdir)
	    ++p;
	  else if (*p != '.')
	    continue;
	  o = variable_buffer_output (o, p, len - (p - p2));
	}
#ifdef HAVE_DOS_PATHS
      /* Handle the case of "d:foo/bar".  */
      else if (streq (funcname, "notdir") && p2[0] && p2[1] == ':')
	{
	  p = p2 + 2;
	  o = variable_buffer_output (o, p, len - (p - p2));
	}
#endif
      else if (is_notdir)
	o = variable_buffer_output (o, p2, len);

      if (is_notdir || p >= p2)
	{
	  o = variable_buffer_output (o, " ", 1);
	  doneany = 1;
	}
    }

  if (doneany)
    /* Kill last space.  */
    --o;

  return o;
}


static char *
func_basename_dir (char *o, char **argv, const char *funcname)
{
  /* Expand the argument.  */
  const char *p3 = argv[0];
  const char *p2;
  int doneany=0;
  unsigned int len=0;

  int is_basename= streq (funcname, "basename");
  int is_dir= !is_basename;

  while ((p2 = find_next_token (&p3, &len)) != 0)
    {
      const char *p = p2 + len;
      while (p >= p2 && (!is_basename  || *p != '.'))
        {
          if (IS_PATHSEP (*p))
            break;
          --p;
        }

      if (p >= p2 && (is_dir))
        o = variable_buffer_output (o, p2, ++p - p2);
      else if (p >= p2 && (*p == '.'))
        o = variable_buffer_output (o, p2, p - p2);
#ifdef HAVE_DOS_PATHS
      /* Handle the "d:foobar" case */
      else if (p2[0] && p2[1] == ':' && is_dir)
        o = variable_buffer_output (o, p2, 2);
#endif
      else if (is_dir)
#ifdef VMS
        o = variable_buffer_output (o, "[]", 2);
#else
#ifndef _AMIGA
      o = variable_buffer_output (o, "./", 2);
#else
      ; /* Just a nop...  */
#endif /* AMIGA */
#endif /* !VMS */
      else
        /* The entire name is the basename.  */
        o = variable_buffer_output (o, p2, len);

      o = variable_buffer_output (o, " ", 1);
      doneany = 1;
    }

  if (doneany)
    /* Kill last space.  */
    --o;

  return o;
}

static char *
func_addsuffix_addprefix (char *o, char **argv, const char *funcname)
{
  int fixlen = strlen (argv[0]);
  const char *list_iterator = argv[1];
  int is_addprefix = streq (funcname, "addprefix");
  int is_addsuffix = !is_addprefix;

  int doneany = 0;
  const char *p;
  unsigned int len;

  while ((p = find_next_token (&list_iterator, &len)) != 0)
    {
      if (is_addprefix)
	o = variable_buffer_output (o, argv[0], fixlen);
      o = variable_buffer_output (o, p, len);
      if (is_addsuffix)
	o = variable_buffer_output (o, argv[0], fixlen);
      o = variable_buffer_output (o, " ", 1);
      doneany = 1;
    }

  if (doneany)
    /* Kill last space.  */
    --o;

  return o;
}

static char *
func_subst (char *o, char **argv, const char *funcname UNUSED)
{
  o = subst_expand (o, argv[2], argv[0], argv[1], strlen (argv[0]),
		    strlen (argv[1]), 0);

  return o;
}


static char *
func_firstword (char *o, char **argv, const char *funcname UNUSED)
{
  unsigned int i;
  const char *words = argv[0];    /* Use a temp variable for find_next_token */
  const char *p = find_next_token (&words, &i);

  if (p != 0)
    o = variable_buffer_output (o, p, i);

  return o;
}

static char *
func_lastword (char *o, char **argv, const char *funcname UNUSED)
{
  unsigned int i;
  const char *words = argv[0];    /* Use a temp variable for find_next_token */
  const char *p = NULL;
  const char *t;

  while ((t = find_next_token (&words, &i)))
    p = t;

  if (p != 0)
    o = variable_buffer_output (o, p, i);

  return o;
}

static char *
func_words (char *o, char **argv, const char *funcname UNUSED)
{
  int i = 0;
  const char *word_iterator = argv[0];
  char buf[20];

  while (find_next_token (&word_iterator, (unsigned int *) 0) != 0)
    ++i;

  sprintf (buf, "%d", i);
  o = variable_buffer_output (o, buf, strlen (buf));

  return o;
}

/* Set begpp to point to the first non-whitespace character of the string,
 * and endpp to point to the last non-whitespace character of the string.
 * If the string is empty or contains nothing but whitespace, endpp will be
 * begpp-1.
 */
char *
strip_whitespace (const char **begpp, const char **endpp)
{
  while (*begpp <= *endpp && isspace ((unsigned char)**begpp))
    (*begpp) ++;
  while (*endpp >= *begpp && isspace ((unsigned char)**endpp))
    (*endpp) --;
  return (char *)*begpp;
}

static void
check_numeric (const char *s, const char *msg)
{
  const char *end = s + strlen (s) - 1;
  const char *beg = s;
  strip_whitespace (&s, &end);

  for (; s <= end; ++s)
    if (!ISDIGIT (*s))  /* ISDIGIT only evals its arg once: see make.h.  */
      break;

  if (s <= end || end - beg < 0)
    fatal (*expanding_var, "%s: '%s'", msg, beg);
}



static char *
func_word (char *o, char **argv, const char *funcname UNUSED)
{
  const char *end_p;
  const char *p;
  int i;

  /* Check the first argument.  */
  check_numeric (argv[0], _("non-numeric first argument to `word' function"));
  i = atoi (argv[0]);

  if (i == 0)
    fatal (*expanding_var,
           _("first argument to `word' function must be greater than 0"));

  end_p = argv[1];
  while ((p = find_next_token (&end_p, 0)) != 0)
    if (--i == 0)
      break;

  if (i == 0)
    o = variable_buffer_output (o, p, end_p - p);

  return o;
}

static char *
func_wordlist (char *o, char **argv, const char *funcname UNUSED)
{
  int start, count;

  /* Check the arguments.  */
  check_numeric (argv[0],
		 _("non-numeric first argument to `wordlist' function"));
  check_numeric (argv[1],
		 _("non-numeric second argument to `wordlist' function"));

  start = atoi (argv[0]);
  if (start < 1)
    fatal (*expanding_var,
           "invalid first argument to `wordlist' function: `%d'", start);

  count = atoi (argv[1]) - start + 1;

  if (count > 0)
    {
      const char *p;
      const char *end_p = argv[2];

      /* Find the beginning of the "start"th word.  */
      while (((p = find_next_token (&end_p, 0)) != 0) && --start)
        ;

      if (p)
        {
          /* Find the end of the "count"th word from start.  */
          while (--count && (find_next_token (&end_p, 0) != 0))
            ;

          /* Return the stuff in the middle.  */
          o = variable_buffer_output (o, p, end_p - p);
        }
    }

  return o;
}

static char *
func_findstring (char *o, char **argv, const char *funcname UNUSED)
{
  /* Find the first occurrence of the first string in the second.  */
  if (strstr (argv[1], argv[0]) != 0)
    o = variable_buffer_output (o, argv[0], strlen (argv[0]));

  return o;
}

static char *
func_foreach (char *o, char **argv, const char *funcname UNUSED)
{
  /* expand only the first two.  */
  char *varname = expand_argument (argv[0], NULL);
  char *list = expand_argument (argv[1], NULL);
  const char *body = argv[2];

  int doneany = 0;
  const char *list_iterator = list;
  const char *p;
  unsigned int len;
  struct variable *var;

  push_new_variable_scope ();
  var = define_variable (varname, strlen (varname), "", o_automatic, 0);

  /* loop through LIST,  put the value in VAR and expand BODY */
  while ((p = find_next_token (&list_iterator, &len)) != 0)
    {
      char *result = 0;
#ifdef CONFIG_WITH_VALUE_LENGTH
      if (len >= (unsigned int)var->value_alloc_len)
        {
          free (var->value);
          var->value_alloc_len = (len + 32) & ~31;
          var->value = xmalloc (var->value_alloc_len);
        }
      memcpy (var->value, p, len);
      var->value[len] = '\0';
      var->value_length = len;
#else
      free (var->value);
      var->value = savestring (p, len);
#endif

      result = allocated_variable_expand (body);

      o = variable_buffer_output (o, result, strlen (result));
      o = variable_buffer_output (o, " ", 1);
      doneany = 1;
      free (result);
    }

  if (doneany)
    /* Kill the last space.  */
    --o;

  pop_variable_scope ();
  free (varname);
  free (list);

  return o;
}

struct a_word
{
  struct a_word *next;
  struct a_word *chain;
  char *str;
  int length;
  int matched;
};

static unsigned long
a_word_hash_1 (const void *key)
{
  return_STRING_HASH_1 (((struct a_word const *) key)->str);
}

static unsigned long
a_word_hash_2 (const void *key)
{
  return_STRING_HASH_2 (((struct a_word const *) key)->str);
}

static int
a_word_hash_cmp (const void *x, const void *y)
{
  int result = ((struct a_word const *) x)->length - ((struct a_word const *) y)->length;
  if (result)
    return result;
  return_STRING_COMPARE (((struct a_word const *) x)->str,
			 ((struct a_word const *) y)->str);
}

struct a_pattern
{
  struct a_pattern *next;
  char *str;
  char *percent;
  int length;
  int save_c;
};

static char *
func_filter_filterout (char *o, char **argv, const char *funcname)
{
  struct a_word *wordhead;
  struct a_word **wordtail;
  struct a_word *wp;
  struct a_pattern *pathead;
  struct a_pattern **pattail;
  struct a_pattern *pp;

  struct hash_table a_word_table;
  int is_filter = streq (funcname, "filter");
  const char *pat_iterator = argv[0];
  const char *word_iterator = argv[1];
  int literals = 0;
  int words = 0;
  int hashing = 0;
  char *p;
  unsigned int len;

  /* Chop ARGV[0] up into patterns to match against the words.  */

  pattail = &pathead;
  while ((p = find_next_token (&pat_iterator, &len)) != 0)
    {
      struct a_pattern *pat = alloca (sizeof (struct a_pattern));

      *pattail = pat;
      pattail = &pat->next;

      if (*pat_iterator != '\0')
	++pat_iterator;

      pat->str = p;
      pat->length = len;
      pat->save_c = p[len];
      p[len] = '\0';
      pat->percent = find_percent (p);
      if (pat->percent == 0)
	literals++;
    }
  *pattail = 0;

  /* Chop ARGV[1] up into words to match against the patterns.  */

  wordtail = &wordhead;
  while ((p = find_next_token (&word_iterator, &len)) != 0)
    {
      struct a_word *word = alloca (sizeof (struct a_word));

      *wordtail = word;
      wordtail = &word->next;

      if (*word_iterator != '\0')
	++word_iterator;

      p[len] = '\0';
      word->str = p;
      word->length = len;
      word->matched = 0;
      word->chain = 0;
      words++;
    }
  *wordtail = 0;

  /* Only use a hash table if arg list lengths justifies the cost.  */
  hashing = (literals >= 2 && (literals * words) >= 10);
  if (hashing)
    {
      hash_init (&a_word_table, words, a_word_hash_1, a_word_hash_2,
                 a_word_hash_cmp);
      for (wp = wordhead; wp != 0; wp = wp->next)
	{
	  struct a_word *owp = hash_insert (&a_word_table, wp);
	  if (owp)
	    wp->chain = owp;
	}
    }

  if (words)
    {
      int doneany = 0;

      /* Run each pattern through the words, killing words.  */
      for (pp = pathead; pp != 0; pp = pp->next)
	{
	  if (pp->percent)
	    for (wp = wordhead; wp != 0; wp = wp->next)
	      wp->matched |= pattern_matches (pp->str, pp->percent, wp->str);
	  else if (hashing)
	    {
	      struct a_word a_word_key;
	      a_word_key.str = pp->str;
	      a_word_key.length = pp->length;
	      wp = hash_find_item (&a_word_table, &a_word_key);
	      while (wp)
		{
		  wp->matched |= 1;
		  wp = wp->chain;
		}
	    }
	  else
	    for (wp = wordhead; wp != 0; wp = wp->next)
	      wp->matched |= (wp->length == pp->length
			      && strneq (pp->str, wp->str, wp->length));
	}

      /* Output the words that matched (or didn't, for filter-out).  */
      for (wp = wordhead; wp != 0; wp = wp->next)
	if (is_filter ? wp->matched : !wp->matched)
	  {
	    o = variable_buffer_output (o, wp->str, strlen (wp->str));
	    o = variable_buffer_output (o, " ", 1);
	    doneany = 1;
	  }

      if (doneany)
	/* Kill the last space.  */
	--o;
    }

  for (pp = pathead; pp != 0; pp = pp->next)
    pp->str[pp->length] = pp->save_c;

  if (hashing)
    hash_free (&a_word_table, 0);

  return o;
}


static char *
func_strip (char *o, char **argv, const char *funcname UNUSED)
{
  const char *p = argv[0];
  int doneany = 0;

  while (*p != '\0')
    {
      int i=0;
      const char *word_start;

      while (isspace ((unsigned char)*p))
	++p;
      word_start = p;
      for (i=0; *p != '\0' && !isspace ((unsigned char)*p); ++p, ++i)
	{}
      if (!i)
	break;
      o = variable_buffer_output (o, word_start, i);
      o = variable_buffer_output (o, " ", 1);
      doneany = 1;
    }

  if (doneany)
    /* Kill the last space.  */
    --o;

  return o;
}

/*
  Print a warning or fatal message.
*/
static char *
func_error (char *o, char **argv, const char *funcname)
{
  char **argvp;
  char *msg, *p;
  int len;

  /* The arguments will be broken on commas.  Rather than create yet
     another special case where function arguments aren't broken up,
     just create a format string that puts them back together.  */
  for (len=0, argvp=argv; *argvp != 0; ++argvp)
    len += strlen (*argvp) + 2;

  p = msg = alloca (len + 1);

  for (argvp=argv; argvp[1] != 0; ++argvp)
    {
      strcpy (p, *argvp);
      p += strlen (*argvp);
      *(p++) = ',';
      *(p++) = ' ';
    }
  strcpy (p, *argvp);

  switch (*funcname) {
    case 'e':
      fatal (reading_file, "%s", msg);

    case 'w':
      error (reading_file, "%s", msg);
      break;

    case 'i':
      printf ("%s\n", msg);
      fflush(stdout);
      break;

    default:
      fatal (*expanding_var, "Internal error: func_error: '%s'", funcname);
  }

  /* The warning function expands to the empty string.  */
  return o;
}


/*
  chop argv[0] into words, and sort them.
 */
static char *
func_sort (char *o, char **argv, const char *funcname UNUSED)
{
  const char *t;
  char **words;
  int wordi;
  char *p;
  unsigned int len;
  int i;

  /* Find the maximum number of words we'll have.  */
  t = argv[0];
  wordi = 1;
  while (*t != '\0')
    {
      char c = *(t++);

      if (! isspace ((unsigned char)c))
        continue;

      ++wordi;

      while (isspace ((unsigned char)*t))
        ++t;
    }

  words = xmalloc (wordi * sizeof (char *));

  /* Now assign pointers to each string in the array.  */
  t = argv[0];
  wordi = 0;
  while ((p = find_next_token (&t, &len)) != 0)
    {
      ++t;
      p[len] = '\0';
      words[wordi++] = p;
    }

  if (wordi)
    {
      /* Now sort the list of words.  */
      qsort (words, wordi, sizeof (char *), alpha_compare);

      /* Now write the sorted list, uniquified.  */
#ifdef CONFIG_WITH_RSORT
      if (strcmp (funcname, "rsort"))
        {
          /* sort */
#endif
          for (i = 0; i < wordi; ++i)
            {
              len = strlen (words[i]);
              if (i == wordi - 1 || strlen (words[i + 1]) != len
                  || strcmp (words[i], words[i + 1]))
                {
                  o = variable_buffer_output (o, words[i], len);
                  o = variable_buffer_output (o, " ", 1);
                }
            }
#ifdef CONFIG_WITH_RSORT
        }
      else
        {
          /* rsort - reverse the result */
          i = wordi;
          while (i-- > 0)
            {
              len = strlen (words[i]);
              if (i == 0 || strlen (words[i - 1]) != len
                  || strcmp (words[i], words[i - 1]))
                {
                  o = variable_buffer_output (o, words[i], len);
                  o = variable_buffer_output (o, " ", 1);
                }
            }
        }
#endif

      /* Kill the last space.  */
      --o;
    }

  free (words);

  return o;
}

/*
  $(if condition,true-part[,false-part])

  CONDITION is false iff it evaluates to an empty string.  White
  space before and after condition are stripped before evaluation.

  If CONDITION is true, then TRUE-PART is evaluated, otherwise FALSE-PART is
  evaluated (if it exists).  Because only one of the two PARTs is evaluated,
  you can use $(if ...) to create side-effects (with $(shell ...), for
  example).
*/

static char *
func_if (char *o, char **argv, const char *funcname UNUSED)
{
  const char *begp = argv[0];
  const char *endp = begp + strlen (argv[0]) - 1;
  int result = 0;

  /* Find the result of the condition: if we have a value, and it's not
     empty, the condition is true.  If we don't have a value, or it's the
     empty string, then it's false.  */

  strip_whitespace (&begp, &endp);

  if (begp <= endp)
    {
      char *expansion = expand_argument (begp, endp+1);

      result = strlen (expansion);
      free (expansion);
    }

  /* If the result is true (1) we want to eval the first argument, and if
     it's false (0) we want to eval the second.  If the argument doesn't
     exist we do nothing, otherwise expand it and add to the buffer.  */

  argv += 1 + !result;

  if (*argv)
    {
      char *expansion = expand_argument (*argv, NULL);

      o = variable_buffer_output (o, expansion, strlen (expansion));

      free (expansion);
    }

  return o;
}

/*
  $(or condition1[,condition2[,condition3[...]]])

  A CONDITION is false iff it evaluates to an empty string.  White
  space before and after CONDITION are stripped before evaluation.

  CONDITION1 is evaluated.  If it's true, then this is the result of
  expansion.  If it's false, CONDITION2 is evaluated, and so on.  If none of
  the conditions are true, the expansion is the empty string.

  Once a CONDITION is true no further conditions are evaluated
  (short-circuiting).
*/

static char *
func_or (char *o, char **argv, const char *funcname UNUSED)
{
  for ( ; *argv ; ++argv)
    {
      const char *begp = *argv;
      const char *endp = begp + strlen (*argv) - 1;
      char *expansion;
      int result = 0;

      /* Find the result of the condition: if it's false keep going.  */

      strip_whitespace (&begp, &endp);

      if (begp > endp)
        continue;

      expansion = expand_argument (begp, endp+1);
      result = strlen (expansion);

      /* If the result is false keep going.  */
      if (!result)
        {
          free (expansion);
          continue;
        }

      /* It's true!  Keep this result and return.  */
      o = variable_buffer_output (o, expansion, result);
      free (expansion);
      break;
    }

  return o;
}

/*
  $(and condition1[,condition2[,condition3[...]]])

  A CONDITION is false iff it evaluates to an empty string.  White
  space before and after CONDITION are stripped before evaluation.

  CONDITION1 is evaluated.  If it's false, then this is the result of
  expansion.  If it's true, CONDITION2 is evaluated, and so on.  If all of
  the conditions are true, the expansion is the result of the last condition.

  Once a CONDITION is false no further conditions are evaluated
  (short-circuiting).
*/

static char *
func_and (char *o, char **argv, const char *funcname UNUSED)
{
  char *expansion;
  int result;

  while (1)
    {
      const char *begp = *argv;
      const char *endp = begp + strlen (*argv) - 1;

      /* An empty condition is always false.  */
      strip_whitespace (&begp, &endp);
      if (begp > endp)
        return o;

      expansion = expand_argument (begp, endp+1);
      result = strlen (expansion);

      /* If the result is false, stop here: we're done.  */
      if (!result)
        break;

      /* Otherwise the result is true.  If this is the last one, keep this
         result and quit.  Otherwise go on to the next one!  */

      if (*(++argv))
        free (expansion);
      else
        {
          o = variable_buffer_output (o, expansion, result);
          break;
        }
    }

  free (expansion);

  return o;
}

static char *
func_wildcard (char *o, char **argv, const char *funcname UNUSED)
{
#ifdef _AMIGA
   o = wildcard_expansion (argv[0], o);
#else
   char *p = string_glob (argv[0]);
   o = variable_buffer_output (o, p, strlen (p));
#endif
   return o;
}

/*
  $(eval <makefile string>)

  Always resolves to the empty string.

  Treat the arguments as a segment of makefile, and parse them.
*/

static char *
func_eval (char *o, char **argv, const char *funcname UNUSED)
{
  char *buf;
  unsigned int len;

  /* Eval the buffer.  Pop the current variable buffer setting so that the
     eval'd code can use its own without conflicting.  */

  install_variable_buffer (&buf, &len);

  eval_buffer (argv[0]);

  restore_variable_buffer (buf, len);

  return o;
}


static char *
func_value (char *o, char **argv, const char *funcname UNUSED)
{
  /* Look up the variable.  */
  struct variable *v = lookup_variable (argv[0], strlen (argv[0]));

  /* Copy its value into the output buffer without expanding it.  */
  if (v)
#ifdef CONFIG_WITH_VALUE_LENGTH
    o = variable_buffer_output (o, v->value,
                                v->value_length >= 0 ? v->value_length : strlen(v->value));
#else
    o = variable_buffer_output (o, v->value, strlen(v->value));
#endif

  return o;
}

/*
  \r  is replaced on UNIX as well. Is this desirable?
 */
static void
fold_newlines (char *buffer, unsigned int *length)
{
  char *dst = buffer;
  char *src = buffer;
  char *last_nonnl = buffer -1;
  src[*length] = 0;
  for (; *src != '\0'; ++src)
    {
      if (src[0] == '\r' && src[1] == '\n')
	continue;
      if (*src == '\n')
	{
	  *dst++ = ' ';
	}
      else
	{
	  last_nonnl = dst;
	  *dst++ = *src;
	}
    }
  *(++last_nonnl) = '\0';
  *length = last_nonnl - buffer;
}



int shell_function_pid = 0, shell_function_completed;


#ifdef WINDOWS32
/*untested*/

#include <windows.h>
#include <io.h>
#include "sub_proc.h"


void
windows32_openpipe (int *pipedes, int *pid_p, char **command_argv, char **envp)
{
  SECURITY_ATTRIBUTES saAttr;
  HANDLE hIn;
  HANDLE hErr;
  HANDLE hChildOutRd;
  HANDLE hChildOutWr;
  HANDLE hProcess;


  saAttr.nLength = sizeof (SECURITY_ATTRIBUTES);
  saAttr.bInheritHandle = TRUE;
  saAttr.lpSecurityDescriptor = NULL;

  if (DuplicateHandle (GetCurrentProcess(),
		      GetStdHandle(STD_INPUT_HANDLE),
		      GetCurrentProcess(),
		      &hIn,
		      0,
		      TRUE,
		      DUPLICATE_SAME_ACCESS) == FALSE) {
    fatal (NILF, _("create_child_process: DuplicateHandle(In) failed (e=%ld)\n"),
	   GetLastError());

  }
  if (DuplicateHandle(GetCurrentProcess(),
		      GetStdHandle(STD_ERROR_HANDLE),
		      GetCurrentProcess(),
		      &hErr,
		      0,
		      TRUE,
		      DUPLICATE_SAME_ACCESS) == FALSE) {
    fatal (NILF, _("create_child_process: DuplicateHandle(Err) failed (e=%ld)\n"),
	   GetLastError());
  }

  if (!CreatePipe(&hChildOutRd, &hChildOutWr, &saAttr, 0))
    fatal (NILF, _("CreatePipe() failed (e=%ld)\n"), GetLastError());

  hProcess = process_init_fd(hIn, hChildOutWr, hErr);

  if (!hProcess)
    fatal (NILF, _("windows32_openpipe (): process_init_fd() failed\n"));

  /* make sure that CreateProcess() has Path it needs */
  sync_Path_environment();

  if (!process_begin(hProcess, command_argv, envp, command_argv[0], NULL)) {
    /* register process for wait */
    process_register(hProcess);

    /* set the pid for returning to caller */
    *pid_p = (int) hProcess;

  /* set up to read data from child */
  pipedes[0] = _open_osfhandle((long) hChildOutRd, O_RDONLY);

  /* this will be closed almost right away */
  pipedes[1] = _open_osfhandle((long) hChildOutWr, O_APPEND);
  } else {
    /* reap/cleanup the failed process */
	process_cleanup(hProcess);

    /* close handles which were duplicated, they weren't used */
	CloseHandle(hIn);
	CloseHandle(hErr);

	/* close pipe handles, they won't be used */
	CloseHandle(hChildOutRd);
	CloseHandle(hChildOutWr);

    /* set status for return */
    pipedes[0] = pipedes[1] = -1;
    *pid_p = -1;
  }
}
#endif


#ifdef __MSDOS__
FILE *
msdos_openpipe (int* pipedes, int *pidp, char *text)
{
  FILE *fpipe=0;
  /* MSDOS can't fork, but it has `popen'.  */
  struct variable *sh = lookup_variable ("SHELL", 5);
  int e;
  extern int dos_command_running, dos_status;

  /* Make sure not to bother processing an empty line.  */
  while (isblank ((unsigned char)*text))
    ++text;
  if (*text == '\0')
    return 0;

  if (sh)
    {
      char buf[PATH_MAX + 7];
      /* This makes sure $SHELL value is used by $(shell), even
	 though the target environment is not passed to it.  */
      sprintf (buf, "SHELL=%s", sh->value);
      putenv (buf);
    }

  e = errno;
  errno = 0;
  dos_command_running = 1;
  dos_status = 0;
  /* If dos_status becomes non-zero, it means the child process
     was interrupted by a signal, like SIGINT or SIGQUIT.  See
     fatal_error_signal in commands.c.  */
  fpipe = popen (text, "rt");
  dos_command_running = 0;
  if (!fpipe || dos_status)
    {
      pipedes[0] = -1;
      *pidp = -1;
      if (dos_status)
	errno = EINTR;
      else if (errno == 0)
	errno = ENOMEM;
      shell_function_completed = -1;
    }
  else
    {
      pipedes[0] = fileno (fpipe);
      *pidp = 42; /* Yes, the Meaning of Life, the Universe, and Everything! */
      errno = e;
      shell_function_completed = 1;
    }
  return fpipe;
}
#endif

/*
  Do shell spawning, with the naughty bits for different OSes.
 */

#ifdef VMS

/* VMS can't do $(shell ...)  */
#define func_shell 0

#else
#ifndef _AMIGA
static char *
func_shell (char *o, char **argv, const char *funcname UNUSED)
{
  char *batch_filename = NULL;

#ifdef __MSDOS__
  FILE *fpipe;
#endif
  char **command_argv;
  const char *error_prefix;
  char **envp;
  int pipedes[2];
  int pid;

#ifndef __MSDOS__
  /* Construct the argument list.  */
  command_argv = construct_command_argv (argv[0], NULL, NULL, &batch_filename);
  if (command_argv == 0)
    return o;
#endif

  /* Using a target environment for `shell' loses in cases like:
     export var = $(shell echo foobie)
     because target_environment hits a loop trying to expand $(var)
     to put it in the environment.  This is even more confusing when
     var was not explicitly exported, but just appeared in the
     calling environment.

     See Savannah bug #10593.

  envp = target_environment (NILF);
  */

  envp = environ;

  /* For error messages.  */
  if (reading_file && reading_file->filenm)
    {
      char *p = alloca (strlen (reading_file->filenm)+11+4);
      sprintf (p, "%s:%lu: ", reading_file->filenm, reading_file->lineno);
      error_prefix = p;
    }
  else
    error_prefix = "";

#if defined(__MSDOS__)
  fpipe = msdos_openpipe (pipedes, &pid, argv[0]);
  if (pipedes[0] < 0)
    {
      perror_with_name (error_prefix, "pipe");
      return o;
    }
#elif defined(WINDOWS32)
  windows32_openpipe (pipedes, &pid, command_argv, envp);
  if (pipedes[0] < 0)
    {
      /* open of the pipe failed, mark as failed execution */
      shell_function_completed = -1;

      return o;
    }
  else
#else
  if (pipe (pipedes) < 0)
    {
      perror_with_name (error_prefix, "pipe");
      return o;
    }

# ifdef __EMX__
  /* close some handles that are unnecessary for the child process */
  CLOSE_ON_EXEC(pipedes[1]);
  CLOSE_ON_EXEC(pipedes[0]);
  /* Never use fork()/exec() here! Use spawn() instead in exec_command() */
  pid = child_execute_job (0, pipedes[1], command_argv, envp);
  if (pid < 0)
    perror_with_name (error_prefix, "spawn");
# else /* ! __EMX__ */
  pid = vfork ();
  if (pid < 0)
    perror_with_name (error_prefix, "fork");
  else if (pid == 0)
    child_execute_job (0, pipedes[1], command_argv, envp);
  else
# endif
#endif
    {
      /* We are the parent.  */
      char *buffer;
      unsigned int maxlen, i;
      int cc;

      /* Record the PID for reap_children.  */
      shell_function_pid = pid;
#ifndef  __MSDOS__
      shell_function_completed = 0;

      /* Free the storage only the child needed.  */
      free (command_argv[0]);
      free (command_argv);

      /* Close the write side of the pipe.  */
# ifdef _MSC_VER /* Avoid annoying msvcrt when debugging. (bird) */
      if (pipedes[1] != -1)
# endif
      close (pipedes[1]);
#endif

      /* Set up and read from the pipe.  */

      maxlen = 200;
      buffer = xmalloc (maxlen + 1);

      /* Read from the pipe until it gets EOF.  */
      for (i = 0; ; i += cc)
	{
	  if (i == maxlen)
	    {
	      maxlen += 512;
	      buffer = xrealloc (buffer, maxlen + 1);
	    }

	  EINTRLOOP (cc, read (pipedes[0], &buffer[i], maxlen - i));
	  if (cc <= 0)
	    break;
	}
      buffer[i] = '\0';

      /* Close the read side of the pipe.  */
#ifdef  __MSDOS__
      if (fpipe)
	(void) pclose (fpipe);
#else
# ifdef _MSC_VER /* Avoid annoying msvcrt when debugging. (bird) */
      if (pipedes[0] != -1)
# endif
      (void) close (pipedes[0]);
#endif

      /* Loop until child_handler or reap_children()  sets
         shell_function_completed to the status of our child shell.  */
      while (shell_function_completed == 0)
	reap_children (1, 0);

      if (batch_filename) {
	DB (DB_VERBOSE, (_("Cleaning up temporary batch file %s\n"),
                       batch_filename));
	remove (batch_filename);
	free (batch_filename);
      }
      shell_function_pid = 0;

      /* The child_handler function will set shell_function_completed
	 to 1 when the child dies normally, or to -1 if it
	 dies with status 127, which is most likely an exec fail.  */

      if (shell_function_completed == -1)
	{
	  /* This likely means that the execvp failed, so we should just
	     write the error message in the pipe from the child.  */
	  fputs (buffer, stderr);
	  fflush (stderr);
	}
      else
	{
	  /* The child finished normally.  Replace all newlines in its output
	     with spaces, and put that in the variable output buffer.  */
	  fold_newlines (buffer, &i);
	  o = variable_buffer_output (o, buffer, i);
	}

      free (buffer);
    }

  return o;
}

#else	/* _AMIGA */

/* Do the Amiga version of func_shell.  */

static char *
func_shell (char *o, char **argv, const char *funcname)
{
  /* Amiga can't fork nor spawn, but I can start a program with
     redirection of my choice.  However, this means that we
     don't have an opportunity to reopen stdout to trap it.  Thus,
     we save our own stdout onto a new descriptor and dup a temp
     file's descriptor onto our stdout temporarily.  After we
     spawn the shell program, we dup our own stdout back to the
     stdout descriptor.  The buffer reading is the same as above,
     except that we're now reading from a file.  */

#include <dos/dos.h>
#include <proto/dos.h>

  BPTR child_stdout;
  char tmp_output[FILENAME_MAX];
  unsigned int maxlen = 200, i;
  int cc;
  char * buffer, * ptr;
  char ** aptr;
  int len = 0;
  char* batch_filename = NULL;

  /* Construct the argument list.  */
  command_argv = construct_command_argv (argv[0], (char **) NULL,
                                         (struct file *) 0, &batch_filename);
  if (command_argv == 0)
    return o;

  /* Note the mktemp() is a security hole, but this only runs on Amiga.
     Ideally we would use main.c:open_tmpfile(), but this uses a special
     Open(), not fopen(), and I'm not familiar enough with the code to mess
     with it.  */
  strcpy (tmp_output, "t:MakeshXXXXXXXX");
  mktemp (tmp_output);
  child_stdout = Open (tmp_output, MODE_NEWFILE);

  for (aptr=command_argv; *aptr; aptr++)
    len += strlen (*aptr) + 1;

  buffer = xmalloc (len + 1);
  ptr = buffer;

  for (aptr=command_argv; *aptr; aptr++)
    {
      strcpy (ptr, *aptr);
      ptr += strlen (ptr) + 1;
      *ptr ++ = ' ';
      *ptr = 0;
    }

  ptr[-1] = '\n';

  Execute (buffer, NULL, child_stdout);
  free (buffer);

  Close (child_stdout);

  child_stdout = Open (tmp_output, MODE_OLDFILE);

  buffer = xmalloc (maxlen);
  i = 0;
  do
    {
      if (i == maxlen)
	{
	  maxlen += 512;
	  buffer = xrealloc (buffer, maxlen + 1);
	}

      cc = Read (child_stdout, &buffer[i], maxlen - i);
      if (cc > 0)
	i += cc;
    } while (cc > 0);

  Close (child_stdout);

  fold_newlines (buffer, &i);
  o = variable_buffer_output (o, buffer, i);
  free (buffer);
  return o;
}
#endif  /* _AMIGA */
#endif  /* !VMS */

#ifdef EXPERIMENTAL

/*
  equality. Return is string-boolean, ie, the empty string is false.
 */
static char *
func_eq (char *o, char **argv, const char *funcname)
{
  int result = ! strcmp (argv[0], argv[1]);
  o = variable_buffer_output (o,  result ? "1" : "", result);
  return o;
}


/*
  string-boolean not operator.
 */
static char *
func_not (char *o, char **argv, const char *funcname)
{
  const char *s = argv[0];
  int result = 0;
  while (isspace ((unsigned char)*s))
    s++;
  result = ! (*s);
  o = variable_buffer_output (o,  result ? "1" : "", result);
  return o;
}
#endif


/* Return the absolute name of file NAME which does not contain any `.',
   `..' components nor any repeated path separators ('/').   */
#ifdef KMK
char *
#else
static char *
#endif
abspath (const char *name, char *apath)
{
  char *dest;
  const char *start, *end, *apath_limit;

  if (name[0] == '\0' || apath == NULL)
    return NULL;

#ifdef WINDOWS32                                                    /* bird */
  dest = w32ify((char *)name, 1);
  if (!dest)
    return NULL;
  {
  size_t len = strlen(dest);
  memcpy(apath, dest, len);
  dest = apath + len;
  }

  (void)end; (void)start; (void)apath_limit;

#elif defined __OS2__                                               /* bird */
  if (_abspath(apath, name, GET_PATH_MAX))
    return NULL;
  dest = strchr(apath, '\0');

  (void)end; (void)start; (void)apath_limit; (void)dest;

#else /* !WINDOWS32 && !__OS2__ */
  apath_limit = apath + GET_PATH_MAX;

#ifdef HAVE_DOS_PATHS /* bird added this */
  if (isalpha(name[0]) && name[1] == ':')
    {
      /* drive spec */
      apath[0] = toupper(name[0]);
      apath[1] = ':';
      apath[2] = '/';
      name += 2;
    }
  else
#endif /* HAVE_DOS_PATHS */
  if (name[0] != '/')
    {
      /* It is unlikely we would make it until here but just to make sure. */
      if (!starting_directory)
	return NULL;

      strcpy (apath, starting_directory);

      dest = strchr (apath, '\0');
    }
  else
    {
      apath[0] = '/';
      dest = apath + 1;
    }

  for (start = end = name; *start != '\0'; start = end)
    {
      unsigned long len;

      /* Skip sequence of multiple path-separators.  */
      while (*start == '/')
	++start;

      /* Find end of path component.  */
      for (end = start; *end != '\0' && *end != '/'; ++end)
        ;

      len = end - start;

      if (len == 0)
	break;
      else if (len == 1 && start[0] == '.')
	/* nothing */;
      else if (len == 2 && start[0] == '.' && start[1] == '.')
	{
	  /* Back up to previous component, ignore if at root already.  */
	  if (dest > apath + 1)
	    while ((--dest)[-1] != '/');
	}
      else
	{
	  if (dest[-1] != '/')
            *dest++ = '/';

	  if (dest + len >= apath_limit)
            return NULL;

	  dest = memcpy (dest, start, len);
          dest += len;
	  *dest = '\0';
	}
    }
#endif /* !WINDOWS32 && !__OS2__ */

  /* Unless it is root strip trailing separator.  */
#ifdef HAVE_DOS_PATHS /* bird (is this correct? what about UNC?) */
  if (dest > apath + 1 + (apath[0] != '/') && dest[-1] == '/')
#else
  if (dest > apath + 1 && dest[-1] == '/')
#endif
    --dest;

  *dest = '\0';

  return apath;
}


static char *
func_realpath (char *o, char **argv, const char *funcname UNUSED)
{
  /* Expand the argument.  */
  const char *p = argv[0];
  const char *path = 0;
  int doneany = 0;
  unsigned int len = 0;
  PATH_VAR (in);
  PATH_VAR (out);

  while ((path = find_next_token (&p, &len)) != 0)
    {
      if (len < GET_PATH_MAX)
        {
          strncpy (in, path, len);
          in[len] = '\0';

          if (
#ifdef HAVE_REALPATH
              realpath (in, out)
#else
              abspath (in, out)
#endif
             )
            {
              o = variable_buffer_output (o, out, strlen (out));
              o = variable_buffer_output (o, " ", 1);
              doneany = 1;
            }
        }
    }

  /* Kill last space.  */
  if (doneany)
    --o;

  return o;
}

static char *
func_abspath (char *o, char **argv, const char *funcname UNUSED)
{
  /* Expand the argument.  */
  const char *p = argv[0];
  const char *path = 0;
  int doneany = 0;
  unsigned int len = 0;
  PATH_VAR (in);
  PATH_VAR (out);

  while ((path = find_next_token (&p, &len)) != 0)
    {
      if (len < GET_PATH_MAX)
        {
          strncpy (in, path, len);
          in[len] = '\0';

          if (abspath (in, out))
            {
              o = variable_buffer_output (o, out, strlen (out));
              o = variable_buffer_output (o, " ", 1);
              doneany = 1;
            }
        }
    }

  /* Kill last space.  */
  if (doneany)
    --o;

  return o;
}

#ifdef CONFIG_WITH_ABSPATHEX
/* same as abspath except that the current path is given as the 2nd argument. */
static char *
func_abspathex (char *o, char **argv, const char *funcname UNUSED)
{
  /* Expand the argument.  */
  char *p = argv[0];
  char *cwd = argv[1];
  unsigned int cwd_len = ~0U;
  char *path = 0;
  int doneany = 0;
  unsigned int len = 0;
  PATH_VAR (in);
  PATH_VAR (out);

  while ((path = find_next_token (&p, &len)) != 0)
    {
      if (len < GET_PATH_MAX)
        {
#ifdef HAVE_DOS_PATHS
          if (path[0] != '/' && path[0] != '\\' && (len < 2 || path[1] != ':') && cwd)
#else
          if (path[0] != '/' && cwd)
#endif
            {
              /* relative path, prefix with cwd. */
              if (cwd_len == ~0U)
                cwd_len = strlen (cwd);
              if (cwd_len + len + 1 >= GET_PATH_MAX)
                  continue;
              memcpy (in, cwd, cwd_len);
              in[cwd_len] = '/';
              memcpy (in + cwd_len + 1, path, len);
              in[cwd_len + len + 1] = '\0';
            }
          else
            {
              /* absolute path pass it as-is. */
              memcpy (in, path, len);
              in[len] = '\0';
            }

          if (abspath (in, out))
            {
              o = variable_buffer_output (o, out, strlen (out));
              o = variable_buffer_output (o, " ", 1);
              doneany = 1;
            }
        }
    }

  /* Kill last space.  */
  if (doneany)
    --o;

 return o;
}
#endif

#ifdef CONFIG_WITH_XARGS
/* Create one or more command lines avoiding the max argument
   lenght restriction of the host OS.

   The last argument is the list of arguments that the normal
   xargs command would be fed from stdin.

   The first argument is initial command and it's arguments.

   If there are three or more arguments, the 2nd argument is
   the command and arguments to be used on subsequent
   command lines. Defaults to the initial command.

   If there are four or more arguments, the 3rd argument is
   the command to be used at the final command line. Defaults
   to the sub sequent or initial command .

   A future version of this function may define more arguments
   and therefor anyone specifying six or more arguments will
   cause fatal errors.

   Typical usage is:
        $(xargs ar cas mylib.a,$(objects))
   or
        $(xargs ar cas mylib.a,ar as mylib.a,$(objects))

   It will then create one or more "ar mylib.a ..." command
   lines with proper \n\t separation so it can be used when
   writing rules. */
static char *
func_xargs (char *o, char **argv, const char *funcname)
{
  int argc;
  const char *initial_cmd;
  size_t initial_cmd_len;
  const char *subsequent_cmd;
  size_t subsequent_cmd_len;
  const char *final_cmd;
  size_t final_cmd_len;
  const char *args;
  size_t max_args;
  int i;

#ifdef ARG_MAX
  /* ARG_MAX is a bit unreliable (environment), so drop 25% of the max. */
# define XARGS_MAX  (ARG_MAX - (ARG_MAX / 4))
#else /* FIXME: update configure with a command line length test. */
# define XARGS_MAX  10240
#endif

  argc = 0;
  while (argv[argc])
    argc++;
  if (argc > 4)
    fatal (NILF, _("Too many arguments for $(xargs)!\n"));

  /* first: the initial / default command.*/
  initial_cmd = argv[0];
  while (isspace ((unsigned char)*initial_cmd))
    initial_cmd++;
  max_args = initial_cmd_len = strlen (initial_cmd);

  /* second: the command for the subsequent command lines. defaults to the initial cmd. */
  subsequent_cmd = argc > 2 && argv[1][0] != '\0' ? argv[1] : "";
  while (isspace ((unsigned char)*subsequent_cmd))
    subsequent_cmd++;
  if (*subsequent_cmd)
    {
      subsequent_cmd_len = strlen (subsequent_cmd);
      if (subsequent_cmd_len > max_args)
        max_args = subsequent_cmd_len;
    }
  else
    {
      subsequent_cmd = initial_cmd;
      subsequent_cmd_len = initial_cmd_len;
    }

  /* third: the final command. defaults to the subseq cmd. */
  final_cmd = argc > 3 && argv[2][0] != '\0' ? argv[2] : "";
  while (isspace ((unsigned char)*final_cmd))
    final_cmd++;
  if (*final_cmd)
    {
      final_cmd_len = strlen (final_cmd);
      if (final_cmd_len > max_args)
        max_args = final_cmd_len;
    }
  else
    {
      final_cmd = subsequent_cmd;
      final_cmd_len = subsequent_cmd_len;
    }

  /* last: the arguments to split up into sensible portions. */
  args = argv[argc - 1];

  /* calc the max argument length. */
  if (XARGS_MAX <= max_args + 2)
    fatal (NILF, _("$(xargs): the commands are longer than the max exec argument length. (%lu <= %lu)\n"),
           (unsigned long)XARGS_MAX, (unsigned long)max_args + 2);
  max_args = XARGS_MAX - max_args - 1;

  /* generate the commands. */
  i = 0;
  for (i = 0; ; i++)
    {
      unsigned int len;
      char *iterator = (char *)args;
      const char *end = args;
      const char *cur;
      const char *tmp;

      /* scan the arguments till we reach the end or the max length. */
      while ((cur = find_next_token(&iterator, &len))
          && (size_t)((cur + len) - args) < max_args)
        end = cur + len;
      if (cur && end == args)
        fatal (NILF, _("$(xargs): command + one single arg is too much. giving up.\n"));

      /* emit the command. */
      if (i == 0)
        {
          o = variable_buffer_output (o, (char *)initial_cmd, initial_cmd_len);
          o = variable_buffer_output (o, " ", 1);
        }
      else if (cur)
        {
          o = variable_buffer_output (o, "\n\t", 2);
          o = variable_buffer_output (o, (char *)subsequent_cmd, subsequent_cmd_len);
          o = variable_buffer_output (o, " ", 1);
        }
      else
        {
          o = variable_buffer_output (o, "\n\t", 2);
          o = variable_buffer_output (o, (char *)final_cmd, final_cmd_len);
          o = variable_buffer_output (o, " ", 1);
        }

      tmp = end;
      while (tmp > args && isspace ((unsigned char)tmp[-1])) /* drop trailing spaces. */
        tmp--;
      o = variable_buffer_output (o, (char *)args, tmp - args);


      /* next */
      if (!cur)
        break;
      args = end;
      while (isspace ((unsigned char)*args))
        args++;
    }

  return o;
}
#endif

#ifdef CONFIG_WITH_TOUPPER_TOLOWER
static char *
func_toupper_tolower (char *o, char **argv, const char *funcname)
{
  /* Expand the argument.  */
  const char *p = argv[0];
  while (*p)
    {
      /* convert to temporary buffer */
      char tmp[256];
      unsigned int i;
      if (!strcmp(funcname, "toupper"))
        for (i = 0; i < sizeof(tmp) && *p; i++, p++)
          tmp[i] = toupper(*p);
      else
        for (i = 0; i < sizeof(tmp) && *p; i++, p++)
          tmp[i] = tolower(*p);
      o = variable_buffer_output (o, tmp, i);
    }

  return o;
}
#endif /* CONFIG_WITH_TOUPPER_TOLOWER */

#if defined(CONFIG_WITH_VALUE_LENGTH) && defined(CONFIG_WITH_COMPARE)

/* Strip leading spaces and other things off a command. */
static const char *
comp_cmds_strip_leading (const char *s, const char *e)
{
    while (s < e)
      {
        const char ch = *s;
        if (!isblank (ch)
         && ch != '@'
         && ch != '+'
         && ch != '-')
          break;
        s++;
      }
    return s;
}

/* Worker for func_comp_vars() which is called if the comparision failed.
   It will do the slow command by command comparision of the commands
   when there invoked as comp-cmds. */
static char *
comp_vars_ne (char *o, const char *s1, const char *e1, const char *s2, const char *e2,
              char *ne_retval, const char *funcname)
{
    /* give up at once if not comp-cmds. */
    if (strcmp (funcname, "comp-cmds") != 0)
      o = variable_buffer_output (o, ne_retval, strlen (ne_retval));
    else
      {
        const char * const s1_start = s1;
        int new_cmd = 1;
        int diff;
        for (;;)
          {
            /* if it's a new command, strip leading stuff. */
            if (new_cmd)
              {
                s1 = comp_cmds_strip_leading (s1, e1);
                s2 = comp_cmds_strip_leading (s2, e2);
                new_cmd = 0;
              }
            if (s1 >= e1 || s2 >= e2)
              break;

            /*
             * Inner compare loop which compares one line.
             * FIXME: parse quoting!
             */
            for (;;)
              {
                const char ch1 = *s1;
                const char ch2 = *s2;
                diff = ch1 - ch2;
                if (diff)
                  break;
                if (ch1 == '\n')
                  break;
                assert (ch1 != '\r');

                /* next */
                s1++;
                s2++;
                if (s1 >= e1 || s2 >= e2)
                  break;
              }

            /*
             * If we exited because of a difference try to end-of-command
             * comparision, e.g. ignore trailing spaces.
             */
            if (diff)
              {
                /* strip */
                while (s1 < e1 && isblank (*s1))
                  s1++;
                while (s2 < e2 && isblank (*s2))
                  s2++;
                if (s1 >= e1 || s2 >= e2)
                  break;

                /* compare again and check that it's a newline. */
                if (*s2 != '\n' || *s1 != '\n')
                  break;
              }
            /* Break out if we exited because of EOS. */
            else if (s1 >= e1 || s2 >= e2)
                break;

            /*
             * Detect the end of command lines.
             */
            if (*s1 == '\n')
              new_cmd = s1 == s1_start || s1[-1] != '\\';
            s1++;
            s2++;
          }

        /*
         * Ignore trailing empty lines.
         */
        if (s1 < e1 || s2 < e2)
          {
            while (s1 < e1 && (isblank (*s1) || *s1 == '\n'))
              if (*s1++ == '\n')
                s1 = comp_cmds_strip_leading (s1, e1);
            while (s2 < e2 && (isblank (*s2) || *s2 == '\n'))
              if (*s2++ == '\n')
                s2 = comp_cmds_strip_leading (s2, e2);
          }

        /* emit the result. */
        if (s1 == e1 && s2 == e2)
          o = variable_buffer_output (o, "", 1);
        else
          o = variable_buffer_output (o, ne_retval, strlen (ne_retval));
      }
    return o;
}

/*
    $(comp-vars var1,var2,not-equal-return)
  or
    $(comp-cmds cmd-var1,cmd-var2,not-equal-return)

  Compares the two variables (that's given by name to avoid unnecessary
  expanding) and return the string in the third argument if not equal.
  If equal, nothing is returned.

  comp-vars will to an exact comparision only stripping leading and
  trailing spaces.

  comp-cmds will compare command by command, ignoring not only leading
  and trailing spaces on each line but also leading one leading '@' and '-'.
*/
static char *
func_comp_vars (char *o, char **argv, const char *funcname)
{
  const char *s1, *e1, *x1, *s2, *e2, *x2;
  char *a1 = NULL, *a2 = NULL;
  size_t l, l1, l2;
  struct variable *var1 = lookup_variable (argv[0], strlen (argv[0]));
  struct variable *var2 = lookup_variable (argv[1], strlen (argv[1]));

  /* the simple cases */
  if (var1 == var2)
    return variable_buffer_output (o, "", 1);       /* eq */
  if (!var1 || !var2)
    return variable_buffer_output (o, argv[2], strlen(argv[2]));
  if (var1->value == var2->value)
    return variable_buffer_output (o, "", 1);       /* eq */
  if (!var1->recursive && !var2->recursive)
  {
    if (    var1->value_length == var2->value_length
        &&  !memcmp (var1->value, var2->value, var1->value_length))
      return variable_buffer_output (o, "", 1);     /* eq */

    /* ignore trailing and leading blanks */
    s1 = var1->value;
    while (isblank ((unsigned char) *s1))
      s1++;
    e1 = s1 + var1->value_length;
    while (e1 > s1 && isblank ((unsigned char) e1[-1]))
      e1--;

    s2 = var2->value;
    while (isblank ((unsigned char) *s2))
      s2++;
    e2 = s2 + var2->value_length;
    while (e2 > s2 && isblank ((unsigned char) e2[-1]))
      e2--;

    if (e1 - s1 != e2 - s2)
      return comp_vars_ne (o, s1, e1, s2, e2, argv[2], funcname);
l_simple_compare:
    if (!memcmp (s1, s2, e1 - s1))
      return variable_buffer_output (o, "", 1);     /* eq */
    return comp_vars_ne (o, s1, e1, s2, e2, argv[2], funcname);
  }

  /* ignore trailing and leading blanks */
  s1 = var1->value;
  e1 = s1 + var1->value_length;
  while (isblank ((unsigned char) *s1))
    s1++;
  while (e1 > s1 && isblank ((unsigned char) e1[-1]))
    e1--;

  s2 = var2->value;
  e2 = s2 + var2->value_length;
  while (isblank((unsigned char)*s2))
    s2++;
  while (e2 > s2 && isblank ((unsigned char) e2[-1]))
    e2--;

  /* both empty after stripping? */
  if (s1 == e1 && s2 == e2)
    return variable_buffer_output (o, "", 1);       /* eq */

  /* optimist. */
  if (   e1 - s1 == e2 - s2
      && !memcmp(s1, s2, e1 - s1))
    return variable_buffer_output (o, "", 1);       /* eq */

  /* compare up to the first '$' or the end. */
  x1 = var1->recursive ? memchr (s1, '$', e1 - s1) : NULL;
  x2 = var2->recursive ? memchr (s2, '$', e2 - s2) : NULL;
  if (!x1 && !x2)
    return comp_vars_ne (o, s1, e1, s2, e2, argv[2], funcname);

  l1 = x1 ? x1 - s1 : e1 - s1;
  l2 = x2 ? x2 - s2 : e2 - s2;
  l = l1 <= l2 ? l1 : l2;
  if (l && memcmp (s1, s2, l))
    return comp_vars_ne (o, s1, e1, s2, e2, argv[2], funcname);

  /* one or both buffers now require expanding. */
  if (!x1)
    s1 += l;
  else
    {
      s1 = a1 = allocated_variable_expand ((char *)s1 + l);
      if (!l)
        while (isblank ((unsigned char) *s1))
          s1++;
      e1 = strchr (s1, '\0');
      while (e1 > s1 && isblank ((unsigned char) e1[-1]))
        e1--;
    }

  if (!x2)
    s2 += l;
  else
    {
      s2 = a2 = allocated_variable_expand ((char *)s2 + l);
      if (!l)
        while (isblank ((unsigned char) *s2))
          s2++;
      e2 = strchr (s2, '\0');
      while (e2 > s2 && isblank ((unsigned char) e2[-1]))
        e2--;
    }

  /* the final compare */
  if (   e1 - s1 != e2 - s2
      || memcmp (s1, s2, e1 - s1))
      o = comp_vars_ne (o, s1, e1, s2, e2, argv[2], funcname);
  else
      o = variable_buffer_output (o, "", 1);        /* eq */
  if (a1)
    free (a1);
  if (a2)
    free (a2);
  return o;
}
#endif


#ifdef CONFIG_WITH_STACK

/* Push an item (string without spaces). */
static char *
func_stack_push (char *o, char **argv, const char *funcname)
{
    do_variable_definition(NILF, argv[0], argv[1], o_file, f_append, 0 /* !target_var */);
    return o;
}

/* Pops an item off the stack / get the top stack element.
   (This is what's tricky to do in pure GNU make syntax.) */
static char *
func_stack_pop_top (char *o, char **argv, const char *funcname)
{
    struct variable *stack_var;
    const char *stack = argv[0];
    const int return_item = argv[0][sizeof("stack-pop") - 1] == '\0';

    stack_var = lookup_variable (stack, strlen (stack) );
    if (stack_var)
      {
        unsigned int len;
        char *iterator = stack_var->value;
        char *lastitem = NULL;
        char *cur;

        while ((cur = find_next_token (&iterator, &len)))
          lastitem = cur;

        if (lastitem != NULL)
          {
            if (strcmp (funcname, "stack-popv") != 0)
              o = variable_buffer_output (o, lastitem, len);
            if (strcmp (funcname, "stack-top") != 0)
              {
                *lastitem = '\0';
                while (lastitem > stack_var->value && isspace (lastitem[-1]))
                  *--lastitem = '\0';
#ifdef CONFIG_WITH_VALUE_LENGTH
                stack_var->value_length = lastitem - stack_var->value;
#endif
              }
          }
      }
    return o;
}
#endif /* CONFIG_WITH_STACK */

#ifdef CONFIG_WITH_MATH

#include <ctype.h>
#ifdef _MSC_VER
typedef __int64 math_int;
#else
# include <stdint.h>
typedef int64_t math_int;
#endif

/* Converts a string to an integer, causes an error if the format is invalid. */
static math_int
math_int_from_string (const char *str)
{
    const char *start;
    unsigned base = 0;
    int      negative = 0;
    math_int num = 0;

    /* strip spaces */
    while (isspace (*str))
      str++;
    if (!*str)
      {
        error (NILF, _("bad number: empty\n"));
        return 0;
      }
    start = str;

    /* check for +/- */
    while (*str == '+' || *str == '-' || isspace (*str))
        if (*str++ == '-')
          negative = !negative;

    /* check for prefix - we do not accept octal numbers, sorry. */
    if (*str == '0' && (str[1] == 'x' || str[1] == 'X'))
      {
        base = 16;
        str += 2;
      }
    else
      {
        /* look for a hex digit, if not found treat it as decimal */
        const char *p2 = str;
        for ( ; *p2; p2++)
          if (isxdigit (*p2) && !isdigit (*p2) && isascii (*p2) )
            {
              base = 16;
              break;
            }
        if (base == 0)
          base = 10;
      }

    /* must have at least one digit! */
    if (    !isascii (*str)
        ||  !(base == 16 ? isxdigit (*str) : isdigit (*str)) )
      {
        error (NILF, _("bad number: '%s'\n"), start);
        return 0;
      }

    /* convert it! */
    while (*str && !isspace (*str))
      {
        int ch = *str++;
        if (ch >= '0' && ch <= '9')
          ch -= '0';
        else if (base == 16 && ch >= 'a' && ch <= 'f')
          ch -= 'a' - 10;
        else if (base == 16 && ch >= 'A' && ch <= 'F')
          ch -= 'A' - 10;
        else
          {
            error (NILF, _("bad number: '%s' (base=%d, pos=%d)\n"), start, base, str - start);
            return 0;
          }
        num *= base;
        num += ch;
      }

    /* check trailing spaces. */
    while (isspace (*str))
      str++;
    if (*str)
      {
        error (NILF, _("bad number: '%s'\n"), start);
        return 0;
      }

    return negative ? -num : num;
}

/* outputs the number (as a string) into the variable buffer. */
static char *
math_int_to_variable_buffer (char *o, math_int num)
{
    static const char xdigits[17] = "0123456789abcdef";
    int negative;
    char strbuf[24]; /* 16 hex + 2 prefix + sign + term => 20 */
    char *str = &strbuf[sizeof (strbuf) - 1];

    negative = num < 0;
    if (negative)
      num = -num;

    *str-- = '\0';

    do
      {
        *str-- = xdigits[num & 0xf];
        num >>= 4;
      }
    while (num);

    *str-- = 'x';
    *str = '0';

    if (negative)
        *--str = '-';

    return variable_buffer_output (o, str, &strbuf[sizeof (strbuf) - 1] - str);
}

/* Add two or more integer numbers. */
static char *
func_int_add (char *o, char **argv, const char *funcname)
{
    math_int num;
    int i;

    num = math_int_from_string (argv[0]);
    for (i = 1; argv[i]; i++)
      num += math_int_from_string (argv[i]);

    return math_int_to_variable_buffer (o, num);
}

/* Subtract two or more integer numbers. */
static char *
func_int_sub (char *o, char **argv, const char *funcname)
{
    math_int num;
    int i;

    num = math_int_from_string (argv[0]);
    for (i = 1; argv[i]; i++)
      num -= math_int_from_string (argv[i]);

    return math_int_to_variable_buffer (o, num);
}

/* Multiply two or more integer numbers. */
static char *
func_int_mul (char *o, char **argv, const char *funcname)
{
    math_int num;
    int i;

    num = math_int_from_string (argv[0]);
    for (i = 1; argv[i]; i++)
      num *= math_int_from_string (argv[i]);

    return math_int_to_variable_buffer (o, num);
}

/* Divide an integer number by one or more divisors. */
static char *
func_int_div (char *o, char **argv, const char *funcname)
{
    math_int num;
    math_int divisor;
    int i;

    num = math_int_from_string (argv[0]);
    for (i = 1; argv[i]; i++)
      {
        divisor = math_int_from_string (argv[i]);
        if (!divisor)
          {
            error (NILF, _("divide by zero ('%s')\n"), argv[i]);
            return math_int_to_variable_buffer (o, 0);
          }
        num /= divisor;
      }

    return math_int_to_variable_buffer (o, num);
}


/* Divide and return the remainder. */
static char *
func_int_mod (char *o, char **argv, const char *funcname)
{
    math_int num;
    math_int divisor;

    num = math_int_from_string (argv[0]);
    divisor = math_int_from_string (argv[1]);
    if (!divisor)
      {
        error (NILF, _("divide by zero ('%s')\n"), argv[1]);
        return math_int_to_variable_buffer (o, 0);
      }
    num %= divisor;

    return math_int_to_variable_buffer (o, num);
}

/* 2-complement. */
static char *
func_int_not (char *o, char **argv, const char *funcname)
{
    math_int num;

    num = math_int_from_string (argv[0]);
    num = ~num;

    return math_int_to_variable_buffer (o, num);
}

/* Bitwise AND (two or more numbers). */
static char *
func_int_and (char *o, char **argv, const char *funcname)
{
    math_int num;
    int i;

    num = math_int_from_string (argv[0]);
    for (i = 1; argv[i]; i++)
      num &= math_int_from_string (argv[i]);

    return math_int_to_variable_buffer (o, num);
}

/* Bitwise OR (two or more numbers). */
static char *
func_int_or (char *o, char **argv, const char *funcname)
{
    math_int num;
    int i;

    num = math_int_from_string (argv[0]);
    for (i = 1; argv[i]; i++)
      num |= math_int_from_string (argv[i]);

    return math_int_to_variable_buffer (o, num);
}

/* Bitwise XOR (two or more numbers). */
static char *
func_int_xor (char *o, char **argv, const char *funcname)
{
    math_int num;
    int i;

    num = math_int_from_string (argv[0]);
    for (i = 1; argv[i]; i++)
      num ^= math_int_from_string (argv[i]);

    return math_int_to_variable_buffer (o, num);
}

/* Compare two integer numbers. Returns make boolean (true="1"; false=""). */
static char *
func_int_cmp (char *o, char **argv, const char *funcname)
{
    math_int num1;
    math_int num2;
    int rc;

    num1 = math_int_from_string (argv[0]);
    num2 = math_int_from_string (argv[1]);

    funcname += sizeof ("int-") - 1;
    if (!strcmp (funcname, "eq"))
      rc = num1 == num2;
    else if (!strcmp (funcname, "ne"))
      rc = num1 != num2;
    else if (!strcmp (funcname, "gt"))
      rc = num1 > num2;
    else if (!strcmp (funcname, "ge"))
      rc = num1 >= num2;
    else if (!strcmp (funcname, "lt"))
      rc = num1 < num2;
    else /*if (!strcmp (funcname, "le"))*/
      rc = num1 <= num2;

    return variable_buffer_output (o, rc ? "1" : "", rc);
}

#endif /* CONFIG_WITH_MATH */


/* Lookup table for builtin functions.

   This doesn't have to be sorted; we use a straight lookup.  We might gain
   some efficiency by moving most often used functions to the start of the
   table.

   If MAXIMUM_ARGS is 0, that means there is no maximum and all
   comma-separated values are treated as arguments.

   EXPAND_ARGS means that all arguments should be expanded before invocation.
   Functions that do namespace tricks (foreach) don't automatically expand.  */

static char *func_call (char *o, char **argv, const char *funcname);


static struct function_table_entry function_table_init[] =
{
 /* Name/size */                    /* MIN MAX EXP? Function */
  { STRING_SIZE_TUPLE("abspath"),       0,  1,  1,  func_abspath},
  { STRING_SIZE_TUPLE("addprefix"),     2,  2,  1,  func_addsuffix_addprefix},
  { STRING_SIZE_TUPLE("addsuffix"),     2,  2,  1,  func_addsuffix_addprefix},
  { STRING_SIZE_TUPLE("basename"),      0,  1,  1,  func_basename_dir},
  { STRING_SIZE_TUPLE("dir"),           0,  1,  1,  func_basename_dir},
  { STRING_SIZE_TUPLE("notdir"),        0,  1,  1,  func_notdir_suffix},
  { STRING_SIZE_TUPLE("subst"),         3,  3,  1,  func_subst},
  { STRING_SIZE_TUPLE("suffix"),        0,  1,  1,  func_notdir_suffix},
  { STRING_SIZE_TUPLE("filter"),        2,  2,  1,  func_filter_filterout},
  { STRING_SIZE_TUPLE("filter-out"),    2,  2,  1,  func_filter_filterout},
  { STRING_SIZE_TUPLE("findstring"),    2,  2,  1,  func_findstring},
  { STRING_SIZE_TUPLE("firstword"),     0,  1,  1,  func_firstword},
  { STRING_SIZE_TUPLE("flavor"),        0,  1,  1,  func_flavor},
  { STRING_SIZE_TUPLE("join"),          2,  2,  1,  func_join},
  { STRING_SIZE_TUPLE("lastword"),      0,  1,  1,  func_lastword},
  { STRING_SIZE_TUPLE("patsubst"),      3,  3,  1,  func_patsubst},
  { STRING_SIZE_TUPLE("realpath"),      0,  1,  1,  func_realpath},
#ifdef CONFIG_WITH_RSORT
  { STRING_SIZE_TUPLE("rsort"),         0,  1,  1,  func_sort},
#endif
  { STRING_SIZE_TUPLE("shell"),         0,  1,  1,  func_shell},
  { STRING_SIZE_TUPLE("sort"),          0,  1,  1,  func_sort},
  { STRING_SIZE_TUPLE("strip"),         0,  1,  1,  func_strip},
  { STRING_SIZE_TUPLE("wildcard"),      0,  1,  1,  func_wildcard},
  { STRING_SIZE_TUPLE("word"),          2,  2,  1,  func_word},
  { STRING_SIZE_TUPLE("wordlist"),      3,  3,  1,  func_wordlist},
  { STRING_SIZE_TUPLE("words"),         0,  1,  1,  func_words},
  { STRING_SIZE_TUPLE("origin"),        0,  1,  1,  func_origin},
  { STRING_SIZE_TUPLE("foreach"),       3,  3,  0,  func_foreach},
  { STRING_SIZE_TUPLE("call"),          1,  0,  1,  func_call},
  { STRING_SIZE_TUPLE("info"),          0,  1,  1,  func_error},
  { STRING_SIZE_TUPLE("error"),         0,  1,  1,  func_error},
  { STRING_SIZE_TUPLE("warning"),       0,  1,  1,  func_error},
  { STRING_SIZE_TUPLE("if"),            2,  3,  0,  func_if},
  { STRING_SIZE_TUPLE("or"),            1,  0,  0,  func_or},
  { STRING_SIZE_TUPLE("and"),           1,  0,  0,  func_and},
  { STRING_SIZE_TUPLE("value"),         0,  1,  1,  func_value},
  { STRING_SIZE_TUPLE("eval"),          0,  1,  1,  func_eval},
#ifdef EXPERIMENTAL
  { STRING_SIZE_TUPLE("eq"),            2,  2,  1,  func_eq},
  { STRING_SIZE_TUPLE("not"),           0,  1,  1,  func_not},
#endif
#ifdef CONFIG_WITH_TOUPPER_TOLOWER
  { STRING_SIZE_TUPLE("toupper"),       0,  1,  1,  func_toupper_tolower},
  { STRING_SIZE_TUPLE("tolower"),       0,  1,  1,  func_toupper_tolower},
#endif
#ifdef CONFIG_WITH_ABSPATHEX
  { STRING_SIZE_TUPLE("abspathex"),     0,  2,  1,  func_abspathex},
#endif
#ifdef CONFIG_WITH_XARGS
  { STRING_SIZE_TUPLE("xargs"),         2,  0,  1,  func_xargs},
#endif
#if defined(CONFIG_WITH_VALUE_LENGTH) && defined(CONFIG_WITH_COMPARE)
  { STRING_SIZE_TUPLE("comp-vars"),     3,  3,  1,  func_comp_vars},
  { STRING_SIZE_TUPLE("comp-cmds"),     3,  3,  1,  func_comp_vars},
#endif
#ifdef CONFIG_WITH_STACK
  { STRING_SIZE_TUPLE("stack-push"),    2,  2,  1,  func_stack_push},
  { STRING_SIZE_TUPLE("stack-pop"),     1,  1,  1,  func_stack_pop_top},
  { STRING_SIZE_TUPLE("stack-popv"),    1,  1,  1,  func_stack_pop_top},
  { STRING_SIZE_TUPLE("stack-top"),     1,  1,  1,  func_stack_pop_top},
#endif
#ifdef CONFIG_WITH_MATH
  { STRING_SIZE_TUPLE("int-add"),       2,  0,  1,  func_int_add},
  { STRING_SIZE_TUPLE("int-sub"),       2,  0,  1,  func_int_sub},
  { STRING_SIZE_TUPLE("int-mul"),       2,  0,  1,  func_int_mul},
  { STRING_SIZE_TUPLE("int-div"),       2,  0,  1,  func_int_div},
  { STRING_SIZE_TUPLE("int-mod"),       2,  2,  1,  func_int_mod},
  { STRING_SIZE_TUPLE("int-not"),       1,  1,  1,  func_int_not},
  { STRING_SIZE_TUPLE("int-and"),       2,  0,  1,  func_int_and},
  { STRING_SIZE_TUPLE("int-or"),        2,  0,  1,  func_int_or},
  { STRING_SIZE_TUPLE("int-xor"),       2,  0,  1,  func_int_xor},
  { STRING_SIZE_TUPLE("int-eq"),        2,  2,  1,  func_int_cmp},
  { STRING_SIZE_TUPLE("int-ne"),        2,  2,  1,  func_int_cmp},
  { STRING_SIZE_TUPLE("int-gt"),        2,  2,  1,  func_int_cmp},
  { STRING_SIZE_TUPLE("int-ge"),        2,  2,  1,  func_int_cmp},
  { STRING_SIZE_TUPLE("int-lt"),        2,  2,  1,  func_int_cmp},
  { STRING_SIZE_TUPLE("int-le"),        2,  2,  1,  func_int_cmp},
#endif
#ifdef KMK_HELPERS
  { STRING_SIZE_TUPLE("kb-src-tool"),   1,  1,  0,  func_kbuild_source_tool},
  { STRING_SIZE_TUPLE("kb-obj-base"),   1,  1,  0,  func_kbuild_object_base},
  { STRING_SIZE_TUPLE("kb-obj-suff"),   1,  1,  0,  func_kbuild_object_suffix},
  { STRING_SIZE_TUPLE("kb-src-prop"),   4,  4,  0,  func_kbuild_source_prop},
  { STRING_SIZE_TUPLE("kb-src-one"),    0,  1,  0,  func_kbuild_source_one},
#endif
};

#define FUNCTION_TABLE_ENTRIES (sizeof (function_table_init) / sizeof (struct function_table_entry))


/* These must come after the definition of function_table.  */

static char *
expand_builtin_function (char *o, int argc, char **argv,
                         const struct function_table_entry *entry_p)
{
  if (argc < (int)entry_p->minimum_args)
    fatal (*expanding_var,
           _("insufficient number of arguments (%d) to function `%s'"),
           argc, entry_p->name);

  /* I suppose technically some function could do something with no
     arguments, but so far none do, so just test it for all functions here
     rather than in each one.  We can change it later if necessary.  */

  if (!argc)
    return o;

  if (!entry_p->func_ptr)
    fatal (*expanding_var,
           _("unimplemented on this platform: function `%s'"), entry_p->name);

  return entry_p->func_ptr (o, argv, entry_p->name);
}

/* Check for a function invocation in *STRINGP.  *STRINGP points at the
   opening ( or { and is not null-terminated.  If a function invocation
   is found, expand it into the buffer at *OP, updating *OP, incrementing
   *STRINGP past the reference and returning nonzero.  If not, return zero.  */

static int
handle_function2 (const struct function_table_entry *entry_p, char **op, const char **stringp) /* bird split it up. */
{
  char openparen = (*stringp)[0];
  char closeparen = openparen == '(' ? ')' : '}';
  const char *beg;
  const char *end;
  int count = 0;
  char *abeg = NULL;
  char **argv, **argvp;
  int nargs;

  beg = *stringp + 1;

  /* We found a builtin function.  Find the beginning of its arguments (skip
     whitespace after the name).  */

  beg = next_token (beg + entry_p->len);

  /* Find the end of the function invocation, counting nested use of
     whichever kind of parens we use.  Since we're looking, count commas
     to get a rough estimate of how many arguments we might have.  The
     count might be high, but it'll never be low.  */

  for (nargs=1, end=beg; *end != '\0'; ++end)
    if (*end == ',')
      ++nargs;
    else if (*end == openparen)
      ++count;
    else if (*end == closeparen && --count < 0)
      break;

  if (count >= 0)
    fatal (*expanding_var,
	   _("unterminated call to function `%s': missing `%c'"),
	   entry_p->name, closeparen);

  *stringp = end;

  /* Get some memory to store the arg pointers.  */
  argvp = argv = alloca (sizeof (char *) * (nargs + 2));

  /* Chop the string into arguments, then a nul.  As soon as we hit
     MAXIMUM_ARGS (if it's >0) assume the rest of the string is part of the
     last argument.

     If we're expanding, store pointers to the expansion of each one.  If
     not, make a duplicate of the string and point into that, nul-terminating
     each argument.  */

  if (entry_p->expand_args)
    {
      const char *p;
      for (p=beg, nargs=0; p <= end; ++argvp)
        {
          const char *next;

          ++nargs;

          if (nargs == entry_p->maximum_args
              || (! (next = find_next_argument (openparen, closeparen, p, end))))
            next = end;

          *argvp = expand_argument (p, next);
          p = next + 1;
        }
    }
  else
    {
      int len = end - beg;
      char *p, *aend;

      abeg = xmalloc (len+1);
      memcpy (abeg, beg, len);
      abeg[len] = '\0';
      aend = abeg + len;

      for (p=abeg, nargs=0; p <= aend; ++argvp)
        {
          char *next;

          ++nargs;

          if (nargs == entry_p->maximum_args
              || (! (next = find_next_argument (openparen, closeparen, p, aend))))
            next = aend;

          *argvp = p;
          *next = '\0';
          p = next + 1;
        }
    }
  *argvp = NULL;

  /* Finally!  Run the function...  */
  *op = expand_builtin_function (*op, nargs, argv, entry_p);

  /* Free memory.  */
  if (entry_p->expand_args)
    for (argvp=argv; *argvp != 0; ++argvp)
      free (*argvp);
  if (abeg)
    free (abeg);

  return 1;
}

int
handle_function (char **op, const char **stringp) /* bird split it up */
{
  const struct function_table_entry *entry_p = lookup_function (*stringp + 1);
  if (!entry_p)
    return 0;
  return handle_function2 (entry_p, op, stringp);
}


/* User-defined functions.  Expand the first argument as either a builtin
   function or a make variable, in the context of the rest of the arguments
   assigned to $1, $2, ... $N.  $0 is the name of the function.  */

static char *
func_call (char *o, char **argv, const char *funcname UNUSED)
{
  static int max_args = 0;
  char *fname;
  char *cp;
  char *body;
  int flen;
  int i;
  int saved_args;
  const struct function_table_entry *entry_p;
  struct variable *v;

  /* There is no way to define a variable with a space in the name, so strip
     leading and trailing whitespace as a favor to the user.  */
  fname = argv[0];
  while (*fname != '\0' && isspace ((unsigned char)*fname))
    ++fname;

  cp = fname + strlen (fname) - 1;
  while (cp > fname && isspace ((unsigned char)*cp))
    --cp;
  cp[1] = '\0';

  /* Calling nothing is a no-op */
  if (*fname == '\0')
    return o;

  /* Are we invoking a builtin function?  */

  entry_p = lookup_function (fname);
  if (entry_p)
    {
      /* How many arguments do we have?  */
      for (i=0; argv[i+1]; ++i)
        ;
      return expand_builtin_function (o, i, argv+1, entry_p);
    }

  /* Not a builtin, so the first argument is the name of a variable to be
     expanded and interpreted as a function.  Find it.  */
  flen = strlen (fname);

  v = lookup_variable (fname, flen);

  if (v == 0)
    warn_undefined (fname, flen);

  if (v == 0 || *v->value == '\0')
    return o;

  body = alloca (flen + 4);
  body[0] = '$';
  body[1] = '(';
  memcpy (body + 2, fname, flen);
  body[flen+2] = ')';
  body[flen+3] = '\0';

  /* Set up arguments $(1) .. $(N).  $(0) is the function name.  */

  push_new_variable_scope ();

  for (i=0; *argv; ++i, ++argv)
    {
      char num[11];

      sprintf (num, "%d", i);
      define_variable (num, strlen (num), *argv, o_automatic, 0);
    }

  /* If the number of arguments we have is < max_args, it means we're inside
     a recursive invocation of $(call ...).  Fill in the remaining arguments
     in the new scope with the empty value, to hide them from this
     invocation.  */

  for (; i < max_args; ++i)
    {
      char num[11];

      sprintf (num, "%d", i);
      define_variable (num, strlen (num), "", o_automatic, 0);
    }

  /* Expand the body in the context of the arguments, adding the result to
     the variable buffer.  */

  v->exp_count = EXP_COUNT_MAX;

  saved_args = max_args;
  max_args = i;
  o = variable_expand_string (o, body, flen+3);
  max_args = saved_args;

  v->exp_count = 0;

  pop_variable_scope ();

  return o + strlen (o);
}

void
hash_init_function_table (void)
{
  hash_init (&function_table, FUNCTION_TABLE_ENTRIES * 2,
	     function_table_entry_hash_1, function_table_entry_hash_2,
	     function_table_entry_hash_cmp);
  hash_load (&function_table, function_table_init,
	     FUNCTION_TABLE_ENTRIES, sizeof (struct function_table_entry));
#ifdef CONFIG_WITH_OPTIMIZATION_HACKS
  {
    unsigned i;
    for (i = 0; i < FUNCTION_TABLE_ENTRIES; i++)
        assert (function_table_init[i].len <= MAX_FUNCTION_LENGTH);
  }
#endif
}