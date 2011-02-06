/* gcm.h
 *
 * Galois counter mode, specified by NIST,
 * http://csrc.nist.gov/publications/nistpubs/800-38D/SP-800-38D.pdf
 *
 */

/* NOTE: Tentative interface, subject to change. No effort will be
   made to avoid incompatible changes. */

/* nettle, low-level cryptographics library
 *
 * Copyright (C) 2011 Niels Möller
 * Copyright (C) 2011 Katholieke Universiteit Leuven
 * 
 * Contributed by Nikos Mavrogiannopoulos
 *
 * The nettle library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 * 
 * The nettle library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with the nettle library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA.
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "gcm.h"

#include "memxor.h"
#include "nettle-internal.h"
#include "macros.h"

#define GHASH_POLYNOMIAL 0xE1

/* Multiplication by 010...0; a big-endian shift right. If the bit
   shifted out is one, the defining polynomial is added to cancel it
   out. */
static void
gcm_rightshift (uint8_t *x)
{
  unsigned long *w = (unsigned long *) x;
  long mask;
  /* Shift uses big-endian representation. */
#if WORDS_BIGENDIAN
# if SIZEOF_LONG == 4
  mask = - (w[3] & 1);
  w[3] = (w[3] >> 1) | ((w[2] & 1) << 31);
  w[2] = (w[2] >> 1) | ((w[1] & 1) << 31);
  w[1] = (w[1] >> 1) | ((w[0] & 1) << 31);
  w[0] = (w[0] >> 1) ^ (mask & (GHASH_POLYNOMIAL << 24)); 
# elif SIZEOF_LONG == 8
  mask = - (w[1] & 1);
  w[1] = (w[1] >> 1) | ((w[0] & 1) << 63);
  w[0] = (w[0] >> 1) ^ (mask & (GHASH_POLYNOMIAL << 56));
# else
#  error Unsupported word size. */
#endif
#else /* ! WORDS_BIGENDIAN */
# if SIZEOF_LONG == 4
#define RSHIFT_WORD(x) \
  ((((x) & 0xfefefefeUL) >> 1) \
   | (((x) & 0x01010101) << 15))
  mask = - ((w[3] >> 24) & 1);
  w[3] = RSHIFT_WORD(w[3]) | ((w[2] >> 17) & 0x80);
  w[2] = RSHIFT_WORD(w[2]) | ((w[1] >> 17) & 0x80);
  w[1] = RSHIFT_WORD(w[1]) | ((w[0] >> 17) & 0x80);
  w[0] = RSHIFT_WORD(w[0]) ^ (mask & GHASH_POLYNOMIAL);
# elif SIZEOF_LONG == 8
#define RSHIFT_WORD(x) \
  ((((x) & 0xfefefefefefefefeUL) >> 1) \
   | (((x) & 0x0101010101010101UL) << 15))
  mask = - ((w[1] >> 56) & 1);
  w[1] = RSHIFT_WORD(w[1]) | ((w[0] >> 49) & 0x80);
  w[0] = RSHIFT_WORD(w[0]) ^ (mask & GHASH_POLYNOMIAL);
# else
#  error Unsupported word size. */
# endif
#endif /* ! WORDS_BIGENDIAN */
}

/* Sets a <- a * b mod r, using the plain bitwise algorithm from the
   specification. */
static void
gcm_gf_mul (uint8_t *x, const uint8_t *y)
{
  uint8_t V[GCM_BLOCK_SIZE];
  uint8_t Z[GCM_BLOCK_SIZE];

  unsigned i;
  memcpy(V, x, sizeof(V));
  memset(Z, 0, sizeof(Z));

  for (i = 0; i < GCM_BLOCK_SIZE; i++)
    {
      uint8_t b = y[i];
      unsigned j;
      for (j = 0; j < 8; j++, b <<= 1)
	{
	  if (b & 0x80)
	    memxor(Z, V, sizeof(V));
	  
	  gcm_rightshift(V);
	}
    }
  memcpy (x, Z, sizeof(Z));
}

/* Increment the rightmost 32 bits. */
#define INC32(block) INCREMENT(4, (block) + GCM_BLOCK_SIZE - 4)

/* Initialization of GCM.
 * @ctx: The context of GCM
 * @cipher: The context of the underlying block cipher
 * @f: The underlying cipher encryption function
 */
void
gcm_set_key(struct gcm_ctx *ctx,
	    void *cipher, nettle_crypt_func f)
{
  memset (ctx->h, 0, sizeof (ctx->h));
  f (cipher, GCM_BLOCK_SIZE, ctx->h, ctx->h);  /* H */
#if GCM_TABLE_BITS
  /* FIXME: Expand hash subkey */
  abort();
#endif
}

/*
 * @length: The size of the iv (fixed for now to GCM_NONCE_SIZE)
 * @iv: The iv
 */
void
gcm_set_iv(struct gcm_ctx *ctx, unsigned length, const uint8_t* iv)
{
  /* FIXME: remove the iv size limitation */
  assert (length == GCM_IV_SIZE);

  memcpy (ctx->iv, iv, GCM_BLOCK_SIZE - 4);
  ctx->iv[GCM_BLOCK_SIZE - 4] = 0;
  ctx->iv[GCM_BLOCK_SIZE - 3] = 0;
  ctx->iv[GCM_BLOCK_SIZE - 2] = 0;
  ctx->iv[GCM_BLOCK_SIZE - 1] = 1;

  memcpy (ctx->ctr, ctx->iv, GCM_BLOCK_SIZE);
  INC32 (ctx->ctr);

  /* Reset the rest of the message-dependent state. */
  memset(ctx->x, 0, sizeof(ctx->x));
  ctx->auth_size = ctx->data_size = 0;
}

static void
gcm_hash(struct gcm_ctx *ctx, unsigned length, const uint8_t *data)
{
  for (; length >= GCM_BLOCK_SIZE;
       length -= GCM_BLOCK_SIZE, data += GCM_BLOCK_SIZE)
    {
      memxor (ctx->x, data, GCM_BLOCK_SIZE);
      gcm_gf_mul (ctx->x, ctx->h);
    }
  if (length > 0)
    {
      memxor (ctx->x, data, length);
      gcm_gf_mul (ctx->x, ctx->h);
    }
}

void
gcm_auth(struct gcm_ctx *ctx,
	 unsigned length, const uint8_t *data)
{
  assert(ctx->auth_size % GCM_BLOCK_SIZE == 0);
  assert(ctx->data_size % GCM_BLOCK_SIZE == 0);

  gcm_hash(ctx, length, data);

  ctx->auth_size += length;
}

static void
gcm_crypt(struct gcm_ctx *ctx, void *cipher, nettle_crypt_func *f,
	  unsigned length,
	   uint8_t *dst, const uint8_t *src)
{
  uint8_t buffer[GCM_BLOCK_SIZE];

  if (src != dst)
    {
      for (; length >= GCM_BLOCK_SIZE;
           (length -= GCM_BLOCK_SIZE,
	    src += GCM_BLOCK_SIZE, dst += GCM_BLOCK_SIZE))
        {
          f (cipher, GCM_BLOCK_SIZE, dst, ctx->ctr);
          memxor (dst, src, GCM_BLOCK_SIZE);
          INC32 (ctx->ctr);
        }
    }
  else
    {
      for (; length >= GCM_BLOCK_SIZE;
           (length -= GCM_BLOCK_SIZE,
	    src += GCM_BLOCK_SIZE, dst += GCM_BLOCK_SIZE))
        {
          f (cipher, GCM_BLOCK_SIZE, buffer, ctx->ctr);
          memxor3 (dst, src, buffer, GCM_BLOCK_SIZE);
          INC32 (ctx->ctr);
        }
    }
  if (length > 0)
    {
      /* A final partial block */
      f (cipher, GCM_BLOCK_SIZE, buffer, ctx->ctr);
      memxor3 (dst, src, buffer, length);
      INC32 (ctx->ctr);
    }
}

void
gcm_encrypt (struct gcm_ctx *ctx, void *cipher, nettle_crypt_func *f,
	     unsigned length,
             uint8_t *dst, const uint8_t *src)
{
  assert(ctx->data_size % GCM_BLOCK_SIZE == 0);

  gcm_crypt(ctx, cipher, f, length, dst, src);
  gcm_hash(ctx, length, dst);

  ctx->data_size += length;
}

void
gcm_decrypt(struct gcm_ctx *ctx, void *cipher, nettle_crypt_func *f,
	    unsigned length, uint8_t *dst, const uint8_t *src)
{
  assert(ctx->data_size % GCM_BLOCK_SIZE == 0);

  gcm_hash(ctx, length, src);
  gcm_crypt(ctx, cipher, f, length, dst, src);

  ctx->data_size += length;
}

void
gcm_digest(struct gcm_ctx *ctx, void *cipher, nettle_crypt_func *f,
	   unsigned length, uint8_t *digest)
{
  uint8_t buffer[GCM_BLOCK_SIZE];

  assert (length <= GCM_BLOCK_SIZE);

  ctx->data_size *= 8;
  ctx->auth_size *= 8;

  WRITE_UINT64 (buffer, ctx->auth_size);
  WRITE_UINT64 (buffer + 8, ctx->data_size);

  gcm_hash(ctx, GCM_BLOCK_SIZE, buffer);

  f (cipher, GCM_BLOCK_SIZE, buffer, ctx->iv);
  memxor3 (digest, ctx->x, buffer, length);

  return;
}