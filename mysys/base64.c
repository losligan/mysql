/* Copyright (c) 2003, 2010, Oracle and/or its affiliates.
   Copyright (c) 2013, MariaDB Foundation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include <my_global.h>
#include <m_string.h>  /* strchr() */
#include <m_ctype.h>  /* my_isspace() */
#include <base64.h>

#ifndef MAIN

static char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                             "abcdefghijklmnopqrstuvwxyz"
                             "0123456789+/";

/**
 * Maximum length base64_needed_encoded_length()
 * can handle without signed integer overflow.
 */
int
base64_encode_max_arg_length()
{
  /*
    base64_needed_encoded_length(1589695686) ->  2147483646 (7FFFFFFE)
    base64_needed_encoded_length(1589695687) -> -2147483645
  */
  return 0x5EC0D4C6; /* 1589695686 */
}


int
base64_needed_encoded_length(int length_of_data)
{
  int nb_base64_chars;
  nb_base64_chars= (length_of_data + 2) / 3 * 4;

  return
    nb_base64_chars +            /* base64 char incl padding */
    (nb_base64_chars - 1)/ 76 +  /* newlines */
    1;                           /* NUL termination of string */
}


/**
 * Maximum length supported by base64_decode().
 */
int
base64_decode_max_arg_length()
{
  return 0x7FFFFFFF;
}


int
base64_needed_decoded_length(int length_of_encoded_data)
{
  return (int) ((longlong) length_of_encoded_data + 3) / 4 * 3;
}


/*
  Encode a data as base64.

  Note: We require that dst is pre-allocated to correct size.
        See base64_needed_encoded_length().

  Note: We add line separators every 76 characters.
  
  Note: The output string is properly padded with the '=' character,
  so the length of the output string is always divisable by 4.
*/

int
base64_encode(const void *src, size_t src_len, char *dst)
{
  const unsigned char *s= (const unsigned char*)src;
  size_t i= 0;
  size_t len= 0;

  for (; i < src_len; len += 4)
  {
    unsigned c;

    if (len == 76)
    {
      len= 0;
      *dst++= '\n';
    }

    c= s[i++];
    c <<= 8;

    if (i < src_len)
      c += s[i];
    c <<= 8;
    i++;

    if (i < src_len)
      c += s[i];
    i++;

    *dst++= base64_table[(c >> 18) & 0x3f];
    *dst++= base64_table[(c >> 12) & 0x3f];

    if (i > (src_len + 1))
      *dst++= '=';
    else
      *dst++= base64_table[(c >> 6) & 0x3f];

    if (i > src_len)
      *dst++= '=';
    else
      *dst++= base64_table[(c >> 0) & 0x3f];
  }
  *dst= '\0';

  return 0;
}


/*
  Base64 decoder stream
*/
typedef struct my_base64_decoder_t
{
  const char *src; /* Pointer to the current input position        */
  const char *end; /* Pointer to the end of input buffer           */
  uint c;          /* Collect bits into this number                */
  int error;       /* Error code                                   */
  uchar state;     /* Character number in the current group of 4   */
  uchar mark;      /* Number of padding marks in the current group */
} MY_BASE64_DECODER;


/*
  Helper table for decoder.
  -2 means "space character"
  -1 means "bad character"
  Non-negative values mean valid base64 encoding character.
*/
static int8
from_base64_table[]=
{
/*00*/  -1,-1,-1,-1,-1,-1,-1,-1,-1,-2,-2,-2,-2,-2,-1,-1,
/*10*/  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
/*20*/  -2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63, /*  !"#$%&'()*+,-./ */
/*30*/  52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1, /* 0123456789:;<=>? */
/*40*/  -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, /* @ABCDEFGHIJKLMNO */
/*50*/  15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1, /* PQRSTUVWXYZ[\]^_ */
/*60*/  -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, /* `abcdefghijklmno */
/*70*/  41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1, /* pqrstuvwxyz{|}~  */
/*80*/  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
/*90*/  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
/*A0*/  -2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
/*B0*/  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
/*C0*/  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
/*D0*/  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
/*E0*/  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
/*F0*/  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};


/**
 * Skip leading spaces in a base64 encoded stream
 * and stop on the first non-space character.
 * decoder->src will point to the first non-space character,
 * or to the end of the input string.
 * In case when end-of-input met on unexpected position,
 * decoder->error is also set to 1.
 *
 * See http://en.wikipedia.org/wiki/Base64 for the base64 encoding details
 *
 * @param  decoder  Pointer to MY_BASE64_DECODER
 *
 * @return
 *   FALSE on success (there are some more non-space input characters)
 *   TRUE  on error (end-of-input found)
 */

static inline my_bool
my_base64_decoder_skip_spaces(MY_BASE64_DECODER *decoder)
{
  for ( ; decoder->src < decoder->end; decoder->src++)
  {
    if (from_base64_table[(uchar) *decoder->src] != -2)
      return FALSE;
  }
  if (decoder->state > 0)
    decoder->error= 1; /* Unexpected end-of-input found */
  return TRUE;
}


/**
 * Convert the next character in a base64 encoded stream
 * to a number in the range [0..63]
 * and mix it with the previously collected value in decoder->c.
 *
 * @param decode base64 decoding stream
 *
 * @return
 *   FALSE on success
 *   TRUE  on error (invalid base64 character found)
 */
static inline my_bool
my_base64_add(MY_BASE64_DECODER *decoder)
{
  int res;
  decoder->c <<= 6;
  if ((res= from_base64_table[(uchar) *decoder->src++]) < 0)
    return (decoder->error= TRUE);
  decoder->c+= (uint) res;
  return FALSE;
}


/**
 * Get the next character from a base64 encoded stream.
 * Skip spaces, then scan the next base64 character or a pad character
 * and collect bits into decoder->c.
 *
 * @param  decoder  Pointer to MY_BASE64_DECODER
 * @return
 *  FALSE on success (a valid base64 encoding character found)
 *  TRUE  on error (unexpected character or unexpected end-of-input found)
 */
static my_bool
my_base64_decoder_getch(MY_BASE64_DECODER *decoder)
{
  if (my_base64_decoder_skip_spaces(decoder))
    return TRUE; /* End-of-input */

  if (!my_base64_add(decoder)) /* Valid base64 character found */
  {
    if (decoder->mark)
    {
      /* If we have scanned '=' already, then only '=' is valid */
      DBUG_ASSERT(decoder->state == 3);
      decoder->error= 1;
      decoder->src--;
      return TRUE; /* expected '=', but encoding character found */
    }
    decoder->state++;
    return FALSE;
  }

  /* Process error */
  switch (decoder->state)
  {
  case 0:
  case 1:
    decoder->src--;
    return TRUE; /* base64 character expected */
    break;

  case 2:
  case 3:
    if (decoder->src[-1] == '=')
    {
      decoder->error= 0; /* Not an error - it's a pad character */
      decoder->mark++;
    }
    else
    {
      decoder->src--;
      return TRUE; /* base64 character or '=' expected */
    }
    break;

  default:
    DBUG_ASSERT(0);
    return TRUE; /* Wrong state, should not happen */
  }

  decoder->state++;
  return FALSE;
}


/**
 * Decode a base64 string
 * The base64-encoded data in the range ['src','*end_ptr') will be
 * decoded and stored starting at 'dst'.  The decoding will stop
 * after 'len' characters have been read from 'src', or when padding
 * occurs in the base64-encoded data. In either case: if 'end_ptr' is
 * non-null, '*end_ptr' will be set to point to the character after
 * the last read character, even in the presence of error.
 *
 * Note: 'dst' must have sufficient space to store the decoded data.
 * Use base64_needed_decoded_length() to calculate the correct space size.
 *
 * Note: we allow spaces and line separators at any position.
 *
 * @param src     Pointer to base64-encoded string
 * @param len     Length of string at 'src'
 * @param dst     Pointer to location where decoded data will be stored
 * @param end_ptr Pointer to variable that will refer to the character
 *                after the end of the encoded data that were decoded.
 *                Can be NULL.
 * @flags         flags e.g. allow multiple chunks
 * @return Number of bytes written at 'dst', or -1 in case of failure
 */
int
base64_decode(const char *src_base, size_t len,
              void *dst, const char **end_ptr, int flags)
{
  char *d= (char*) dst;
  MY_BASE64_DECODER decoder;

  decoder.src= src_base;
  decoder.end= src_base + len;
  decoder.error= 0;
  decoder.mark= 0;

  for ( ; ; )
  {
    decoder.c= 0;
    decoder.state= 0;

    if (my_base64_decoder_getch(&decoder) ||
        my_base64_decoder_getch(&decoder) ||
        my_base64_decoder_getch(&decoder) ||
        my_base64_decoder_getch(&decoder))
      break;

    *d++= (decoder.c >> 16) & 0xff;
    *d++= (decoder.c >>  8) & 0xff;
    *d++= (decoder.c >>  0) & 0xff;

    if (decoder.mark)
    {
      d-= decoder.mark;
      if (!(flags & MY_BASE64_DECODE_ALLOW_MULTIPLE_CHUNKS))
        break;
      decoder.mark= 0;
    }
  }

  /* Return error if there are more non-space characters */
  decoder.state= 0;
  if (!my_base64_decoder_skip_spaces(&decoder))
    decoder.error= 1;

  if (end_ptr != NULL)
    *end_ptr= decoder.src;

  return decoder.error ? -1 : (int) (d - (char*) dst);
}


#else /* MAIN */

#define require(b) { \
  if (!(b)) { \
    printf("Require failed at %s:%d\n", __FILE__, __LINE__); \
    abort(); \
  } \
}


int
main(void)
{
  int i;
  size_t j;
  size_t k, l;
  size_t dst_len;
  size_t needed_length;

  for (i= 0; i < 500; i++)
  {
    /* Create source data */
    const size_t src_len= rand() % 1000 + 1;

    char * src= (char *) malloc(src_len);
    char * s= src;
    char * str;
    char * dst;

    require(src);
    for (j= 0; j<src_len; j++)
    {
      char c= rand();
      *s++= c;
    }

    /* Encode */
    needed_length= base64_needed_encoded_length(src_len);
    str= (char *) malloc(needed_length);
    require(str);
    for (k= 0; k < needed_length; k++)
      str[k]= 0xff; /* Fill memory to check correct NUL termination */
    require(base64_encode(src, src_len, str) == 0);
    require(needed_length == strlen(str) + 1);

    /* Decode */
    dst= (char *) malloc(base64_needed_decoded_length(strlen(str)));
    require(dst);
    dst_len= base64_decode(str, strlen(str), dst, NULL);
    require(dst_len == src_len);

    if (memcmp(src, dst, src_len) != 0)
    {
      printf("       --------- src ---------   --------- dst ---------\n");
      for (k= 0; k<src_len; k+=8)
      {
        printf("%.4x   ", (uint) k);
        for (l=0; l<8 && k+l<src_len; l++)
        {
          unsigned char c= src[k+l];
          printf("%.2x ", (unsigned)c);
        }

        printf("  ");

        for (l=0; l<8 && k+l<dst_len; l++)
        {
          unsigned char c= dst[k+l];
          printf("%.2x ", (unsigned)c);
        }
        printf("\n");
      }
      printf("src length: %.8x, dst length: %.8x\n",
             (uint) src_len, (uint) dst_len);
      require(0);
    }
  }
  printf("Test succeeded.\n");
  return 0;
}

#endif
