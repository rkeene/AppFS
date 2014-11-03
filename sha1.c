/* This code is public-domain - it is based on libcrypt
 * placed in the public domain by Wei Dai and other contributors.
 */
/* http://oauth.googlecode.com/svn/code/c/liboauth/src/sha1.c */

#include <tcl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>

#ifdef __BIG_ENDIAN__
#	define SHA_BIG_ENDIAN
#elif defined __LITTLE_ENDIAN__
#elif defined __BYTE_ORDER
# if __BYTE_ORDER__ ==  __ORDER_BIG_ENDIAN__
# define SHA_BIG_ENDIAN
# endif
#else /* ! defined __LITTLE_ENDIAN__ */
# include <endian.h> /* machine/endian.h */
# if __BYTE_ORDER__ ==  __ORDER_BIG_ENDIAN__
#  define SHA_BIG_ENDIAN
# endif
#endif

/* header */
#define HASH_LENGTH 20
#define BLOCK_LENGTH 64

typedef struct sha1info {
	uint32_t buffer[BLOCK_LENGTH / 4];
	uint32_t state[HASH_LENGTH / 4];
	uint32_t byteCount;
	uint8_t bufferOffset;
	uint8_t keyBuffer[BLOCK_LENGTH];
	uint8_t innerHash[HASH_LENGTH];
} sha1info;

/* public API - prototypes - TODO: doxygen*/

/**
 */
static void sha1_init(sha1info *s);
/**
 */
static void sha1_writebyte(sha1info *s, uint8_t data);
/**
 */
static void sha1_write(sha1info *s, const char *data, size_t len);
/**
 */
static uint8_t *sha1_result(sha1info *s);
/**
 */
static void sha1_initHmac(sha1info *s, const uint8_t *key, int keyLength);
/**
 */
static uint8_t *sha1_resultHmac(sha1info *s);

/* code */
#define SHA1_K0  0x5a827999
#define SHA1_K20 0x6ed9eba1
#define SHA1_K40 0x8f1bbcdc
#define SHA1_K60 0xca62c1d6

static void sha1_init(sha1info *s) {
	s->state[0] = 0x67452301;
	s->state[1] = 0xefcdab89;
	s->state[2] = 0x98badcfe;
	s->state[3] = 0x10325476;
	s->state[4] = 0xc3d2e1f0;
	s->byteCount = 0;
	s->bufferOffset = 0;
}

static uint32_t sha1_rol32(uint32_t number, uint8_t bits) {
	return ((number << bits) | (number >> (32 - bits)));
}

static void sha1_hashBlock(sha1info *s) {
	uint8_t i;
	uint32_t a, b, c, d, e, t;

	a = s->state[0];
	b = s->state[1];
	c = s->state[2];
	d = s->state[3];
	e = s->state[4];
	for (i = 0; i < 80; i++) {
		if (i >= 16) {
			t = s->buffer[(i + 13) & 15] ^ s->buffer[(i + 8) & 15] ^ s->buffer[(i + 2) & 15] ^ s->buffer[i & 15];
			s->buffer[i & 15] = sha1_rol32(t, 1);
		}
		if (i < 20) {
			t = (d ^ (b & (c ^ d))) + SHA1_K0;
		} else if (i < 40) {
			t = (b ^ c ^ d) + SHA1_K20;
		} else if (i < 60) {
			t = ((b & c) | (d & (b | c))) + SHA1_K40;
		} else {
			t = (b ^ c ^ d) + SHA1_K60;
		}
		t += sha1_rol32(a, 5) + e + s->buffer[i & 15];
		e = d;
		d = c;
		c = sha1_rol32(b, 30);
		b = a;
		a = t;
	}
	s->state[0] += a;
	s->state[1] += b;
	s->state[2] += c;
	s->state[3] += d;
	s->state[4] += e;
}

static void sha1_addUncounted(sha1info *s, uint8_t data) {
	uint8_t * const b = (uint8_t *) s->buffer;
#ifdef SHA_BIG_ENDIAN
	b[s->bufferOffset] = data;
#else
	b[s->bufferOffset ^ 3] = data;
#endif
	s->bufferOffset++;
	if (s->bufferOffset == BLOCK_LENGTH) {
		sha1_hashBlock(s);
		s->bufferOffset = 0;
	}
}

static void sha1_writebyte(sha1info *s, uint8_t data) {
	++s->byteCount;
	sha1_addUncounted(s, data);
}

static void sha1_write(sha1info *s, const char *data, size_t len) {
	for (; len--; ) {
		sha1_writebyte(s, (uint8_t) *data++);
	}
}

static void sha1_pad(sha1info *s) {
	/* Implement SHA-1 padding (fips180-2 §5.1.1) */

	/* Pad with 0x80 followed by 0x00 until the end of the block */
	sha1_addUncounted(s, 0x80);
	while (s->bufferOffset != 56) {
		sha1_addUncounted(s, 0x00);
	}

	/* Append length in the last 8 bytes */
	sha1_addUncounted(s, 0); /* We're only using 32 bit lengths */
	sha1_addUncounted(s, 0); /* But SHA-1 supports 64 bit lengths */
	sha1_addUncounted(s, 0); /* So zero pad the top bits */
	sha1_addUncounted(s, s->byteCount >> 29); /* Shifting to multiply by 8 */
	sha1_addUncounted(s, s->byteCount >> 21); /* as SHA-1 supports bitstreams as well as */
	sha1_addUncounted(s, s->byteCount >> 13); /* byte. */
	sha1_addUncounted(s, s->byteCount >> 5);
	sha1_addUncounted(s, s->byteCount << 3);
}

static uint8_t *sha1_result(sha1info *s) {
	int i;

	/* Pad to complete the last block */
	sha1_pad(s);

#ifndef SHA_BIG_ENDIAN
	/* Swap byte order back */
	for (i = 0; i < 5; i++) {
		s->state[i]=
			  (((s->state[i]) << 24) & 0xff000000)
			| (((s->state[i]) <<  8) & 0x00ff0000)
			| (((s->state[i]) >>  8) & 0x0000ff00)
			| (((s->state[i]) >> 24) & 0x000000ff);
	}
#endif
	/* Return pointer to hash (20 characters) */
	return((uint8_t *) s->state);
}

#define HMAC_IPAD 0x36
#define HMAC_OPAD 0x5c

static void sha1_initHmac(sha1info *s, const uint8_t *key, int keyLength) {
	uint8_t i;

	memset(s->keyBuffer, 0, BLOCK_LENGTH);
	if (keyLength > BLOCK_LENGTH) {
		/* Hash long keys */
		sha1_init(s);
		for (; keyLength--; ) {
			sha1_writebyte(s, *key++);
		}
		memcpy(s->keyBuffer, sha1_result(s), HASH_LENGTH);
	} else {
		/* Block length keys are used as is */
		memcpy(s->keyBuffer, key, keyLength);
	}

	/* Start inner hash */
	sha1_init(s);
	for (i=0; i<BLOCK_LENGTH; i++) {
		sha1_writebyte(s, s->keyBuffer[i] ^ HMAC_IPAD);
	}

	return;
}

static uint8_t *sha1_resultHmac(sha1info *s) {
	uint8_t i;

	/* Complete inner hash */
	memcpy(s->innerHash, sha1_result(s), HASH_LENGTH);

	/* Calculate outer hash */
	sha1_init(s);

	for (i = 0; i < BLOCK_LENGTH; i++) {
		sha1_writebyte(s, s->keyBuffer[i] ^ HMAC_OPAD);
	}
	for (i = 0; i < HASH_LENGTH; i++) {
		sha1_writebyte(s, s->innerHash[i]);
	}

	return(sha1_result(s));
}

static Tcl_Obj* c_sha1__sha1_file(char* file) {

	sha1info sha1;
	uint8_t buf[4096];
	int fd;
	ssize_t read_ret;
	Tcl_Obj *ret;

	fd = open(file, O_RDONLY);
	if (fd < 0) {
		return(NULL);
	}

	sha1_init(&sha1);

	while (1) {
		read_ret = read(fd, buf, sizeof(buf));

		if (read_ret == 0) {
			break;
		}

		if (read_ret < 0) {
			close(fd);

			return(NULL);
		}

		sha1_write(&sha1, buf, read_ret);
	}

	close(fd);

	sha1_result(&sha1);

	ret = Tcl_NewByteArrayObj(sha1_result(&sha1), HASH_LENGTH);

	return(ret);
}

static int tcl_sha1__sha1_file(ClientData dummy, Tcl_Interp *ip, int objc, Tcl_Obj *CONST objv[]) {
	char* _file;
	Tcl_Obj* rv;
	if (objc != 2) {
		Tcl_WrongNumArgs(ip, 1, objv, "file");
		return TCL_ERROR;
	}
	_file = Tcl_GetString(objv[1]);

	rv = c_sha1__sha1_file(_file);
	if (rv == NULL) {
		return(TCL_ERROR);
	}
	Tcl_SetObjResult(ip, rv); Tcl_DecrRefCount(rv);
	return TCL_OK;
}

static Tcl_Obj* c_sha1__sha1_string(Tcl_Obj* str) {

	sha1info sha1;
	unsigned char *buf;
	int buf_len;
	Tcl_Obj *ret;

	sha1_init(&sha1);

	buf = Tcl_GetByteArrayFromObj(str, &buf_len);
	if (buf == NULL) {
		return(NULL);
	}

	sha1_write(&sha1, buf, buf_len);

	sha1_result(&sha1);

	ret = Tcl_NewByteArrayObj(sha1_result(&sha1), HASH_LENGTH);

	return(ret);
}

static int tcl_sha1__sha1_string(ClientData dummy, Tcl_Interp *ip, int objc, Tcl_Obj *CONST objv[]) {
	Tcl_Obj* _str;
	Tcl_Obj* rv;
	if (objc != 2) {
		Tcl_WrongNumArgs(ip, 1, objv, "str");
		return TCL_ERROR;
	}
	_str = objv[1];

	rv = c_sha1__sha1_string(_str);
	if (rv == NULL) {
		return(TCL_ERROR);
	}
	Tcl_SetObjResult(ip, rv); Tcl_DecrRefCount(rv);
	return TCL_OK;
}

int Sha1_Init(Tcl_Interp *interp) {
#ifdef USE_TCL_STUBS
	if (Tcl_InitStubs(interp, TCL_VERSION, 0) == 0L) {
		return TCL_ERROR;
	}
#endif
	Tcl_CreateObjCommand(interp, "sha1::_sha1_file", tcl_sha1__sha1_file, NULL, NULL);
	Tcl_CreateObjCommand(interp, "sha1::_sha1_string", tcl_sha1__sha1_string, NULL, NULL);
	Tcl_PkgProvide(interp, "sha1", "1.0");
	return(TCL_OK);
}
