/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "str.h"
#include "imap-quote.h"

void imap_quote_append(string_t *str, const unsigned char *value,
		       size_t value_len)
{
	size_t i, linefeeds = 0;
	int last_lwsp, first_lwsp, literal = FALSE, modify = FALSE;

	if (value == NULL) {
		str_append(str, "NIL");
		return;
	}

	if (value_len == (size_t)-1)
		value_len = strlen((const char *) value);

	i = str_len(str);
	first_lwsp = last_lwsp = i > 0 && str_data(str)[i-1] == ' ';

	for (i = 0; i < value_len; i++) {
		switch (value[i]) {
		case 0:
			/* it's converted to 8bit char */
			literal = TRUE;
		case '\t':
			modify = TRUE;
			break;
		case ' ':
			if (last_lwsp)
				modify = TRUE;
			last_lwsp = TRUE;
			break;
		case 13:
		case 10:
			linefeeds++;
			modify = TRUE;
			break;
		default:
			if ((value[i] & 0x80) != 0 ||
			    value[i] == '"' || value[i] == '\\')
				literal = TRUE;
		}
	}

	if (!literal) {
		/* no 8bit chars or imapspecials, return as "string" */
		str_append_c(str, '"');
	} else {
		/* return as literal */
		str_printfa(str, "{%"PRIuSIZE_T"}\r\n", value_len - linefeeds);
	}

	if (!modify)
		str_append_n(str, value, value_len);
	else {
		last_lwsp = first_lwsp;
		for (i = 0; i < value_len; i++) {
			switch (value[i]) {
			case 0:
				str_append_c(str, 128);
				last_lwsp = FALSE;
				break;
			case ' ':
			case '\t':
				if (!last_lwsp)
					str_append_c(str, ' ');
				last_lwsp = TRUE;
				break;
			case 13:
			case 10:
				break;
			default:
				last_lwsp = FALSE;
				str_append_c(str, value[i]);
				break;
			}
		}
	}

	if (!literal)
		str_append_c(str, '"');
}

char *imap_quote(pool_t pool, const unsigned char *value, size_t value_len)
{
	string_t *str;
	char *ret;

	i_assert(pool != data_stack_pool);

	if (value == NULL)
		return "NIL";

	t_push();
	str = t_str_new(value_len + MAX_INT_STRLEN + 5);
	imap_quote_append(str, value, value_len);
	ret = p_strndup(pool, str_data(str), str_len(str));
	t_pop();

	return ret;
}
