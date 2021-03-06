/***********************************************************************
 *
 * Project:  PgSQL Postal Normalizer
 * Purpose:  Main file.
 *
 ***********************************************************************
 * Copyright 2016 Paul Ramsey <pramsey@cleverelephant.ca>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 ***********************************************************************/

/* PostgreSQL */
#include <postgres.h>

/* Check PgSQL version */
#if PG_VERSION_NUM < 90400
#error PostgreSQL 9.4 or newer is required
#endif

#include <catalog/pg_type.h>
#include <lib/stringinfo.h>
#include <utils/array.h>
#include <utils/builtins.h>
#include <utils/jsonb.h>
#include <utils/lsyscache.h>

/* libpostal */
#include <libpostal/libpostal.h>

/* Set up PgSQL */
PG_MODULE_MAGIC;

/* Function signatures */
void _PG_init(void);
void _PG_fini(void);

/* Startup */
void _PG_init(void)
{
	if (!libpostal_setup() || 
	    !libpostal_setup_parser() ||
	    !libpostal_setup_language_classifier())
	{
		elog(ERROR, "Failed to initialize libpostal.");
	}
}

/* Tear-down */
void _PG_fini(void)
{
	libpostal_teardown();
	libpostal_teardown_parser();
	libpostal_teardown_language_classifier();
}


/**
* Normalization function. Takes single input address string and outputs
* an array of possible normalizations for that string.
*/
Datum postal_normalize(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(postal_normalize);
Datum postal_normalize(PG_FUNCTION_ARGS)
{
	text *address = PG_GETARG_TEXT_P(0);
	size_t arr_nelems, i;
	libpostal_normalize_options_t options = libpostal_get_default_options();
	char **expansions = libpostal_expand_address(text_to_cstring(address), options, &arr_nelems);
	ArrayType *arr;
	Datum *arr_elems = palloc(sizeof(Datum) * arr_nelems);
	
	Oid elem_type = TEXTOID;
	int16 elem_len;
	bool elem_byval;
	char elem_align;
	
	get_typlenbyvalalign(elem_type, &elem_len, &elem_byval, &elem_align);
	
	for (i = 0; i < arr_nelems; i++)
	{
		arr_elems[i] = PointerGetDatum(cstring_to_text(expansions[i]));
	}
	
	/* Array construction takes a full copy of the input */
	arr = construct_array(arr_elems, arr_nelems, elem_type, elem_len, elem_byval, elem_align);
	
	/* Clean up unmanaged memory */
	libpostal_expansion_array_destroy(expansions, arr_nelems);
	
	PG_RETURN_ARRAYTYPE_P(arr); 
}


static char* escape_json_string(const char *str)
{
	size_t str_len = strlen(str);
	size_t pstr_len = 2*(str_len+1);
	int i, j = 0;
	char *pstr = palloc(pstr_len);
	for (i = 0; i < str_len; i++)
	{
		char ch = str[i];
		switch (ch)
		{
		case '\\':
			pstr[j++] = '\\';
			pstr[j++] = '\\';
			break;
		case '"':
			pstr[j++] = '\\';
			pstr[j++] = '"';
			break;
		case '\n':
			pstr[j++] = '\\';
			pstr[j++] = 'n';
			break;
		case '\r':
			pstr[j++] = '\\';
			pstr[j++] = 'r';
			break;
		case '\t':
			pstr[j++] = '\\';
			pstr[j++] = 't';
			break;
		case '\b':
			pstr[j++] = '\\';
			pstr[j++] = 'b';
			break;
		case '\f':
			pstr[j++] = '\\';
			pstr[j++] = 'f';
			break;
		default:
			pstr[j++] = ch;
		}
		if (j == pstr_len) break;
	}
	pstr[j++] = '\0';
	return pstr;
}

/**
* Parsing function. Takes single input address string and outputs
* a JSOB type with the keys set from libpostal.
*/
Datum postal_parse(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(postal_parse);
Datum postal_parse(PG_FUNCTION_ARGS)
{
	text *address = PG_GETARG_TEXT_P(0);
	size_t i;
	libpostal_address_parser_options_t options = libpostal_get_address_parser_default_options();
	libpostal_address_parser_response_t *parsed = libpostal_parse_address(text_to_cstring(address), options);
	StringInfoData strbuf;

	/* 
	* There's no easy way to directly create a JSONB in the 
	* PgSQL external API, so instead we (yuck) just build
	* a JSON string and then have PgSQL parse that.
	*/
	initStringInfo(&strbuf);
	appendStringInfoChar(&strbuf, '{');
	for (i = 0; i < parsed->num_components; i++) 
	{
		char *component;
		if (i > 0) 
		{
			appendStringInfoChar(&strbuf, ',');
		}
		component = escape_json_string(parsed->components[i]);
		appendStringInfo(&strbuf, "\"%s\":\"%s\"", parsed->labels[i], component);
		pfree(component);
	}
	appendStringInfoChar(&strbuf, '}');

	/* Clean up unmanaged memory */
	libpostal_address_parser_response_destroy(parsed);

	/* Call JSONB parser */
	PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(strbuf.data)));
}



// Local Variables:
// mode: C++
// tab-width: 4
// c-basic-offset: 4
// indent-tabs-mode: t
// End:
