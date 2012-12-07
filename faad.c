/* 
 *  Squeezelite - lightweight headless squeezeplay emulator for linux
 *
 *  (c) Adrian Smith 2012, triode1@btinternet.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "squeezelite.h"

#include <neaacdec.h>
#include <dlfcn.h>

#define LIBFAAD "libfaad.so.2"

#define WRAPBUF_LEN 2048

struct chunk_table {
	u32_t sample, offset;
};

struct faad {
	NeAACDecHandle hAac;
	u8_t type;
	// following used for mp4 only
	u32_t consume;
	u32_t pos;
	u32_t sample;
	u32_t nextchunk;
	void *stsc;
	struct chunk_table *chunkinfo;
	// faad symbols to be dynamically loaded
	unsigned long (* NeAACDecGetCapabilities)(void);
	NeAACDecConfigurationPtr (* NeAACDecGetCurrentConfiguration)(NeAACDecHandle);
	unsigned char (* NeAACDecSetConfiguration)(NeAACDecHandle, NeAACDecConfigurationPtr);
	NeAACDecHandle (* NeAACDecOpen)(void);
	void (* NeAACDecClose)(NeAACDecHandle);
	long (* NeAACDecInit)(NeAACDecHandle, unsigned char *, unsigned long, unsigned long *, unsigned char *);
	char (* NeAACDecInit2)(NeAACDecHandle, unsigned char *pBuffer, unsigned long, unsigned long *, unsigned char *);
	void *(* NeAACDecDecode)(NeAACDecHandle, NeAACDecFrameInfo *, unsigned char *, unsigned long);
	char *(* NeAACDecGetErrorMessage)(unsigned char);
};

static struct faad *a;

extern log_level loglevel;

extern struct buffer *streambuf;
extern struct buffer *outputbuf;
extern struct streamstate stream;
extern struct outputstate output;
extern struct decodestate decode;

#define LOCK_S   pthread_mutex_lock(&streambuf->mutex)
#define UNLOCK_S pthread_mutex_unlock(&streambuf->mutex)
#define LOCK_O   pthread_mutex_lock(&outputbuf->mutex)
#define UNLOCK_O pthread_mutex_unlock(&outputbuf->mutex)

typedef u_int32_t frames_t;

// minimal code for mp4 file parsing to extract audio config and find media data

// adapted from faad2/common/mp4ff
u32_t mp4_desc_length(u8_t **buf) {
	u8_t b;
	u8_t num_bytes = 0;
	u32_t length = 0;

	do {
		b = **buf;
		*buf += 1;
		num_bytes++;
		length = (length << 7) | (b & 0x7f);
	} while ((b & 0x80) && num_bytes < 4);

	return length;
}

// read mp4 header to extract config data
static int read_mp4_header(unsigned long *samplerate_p, unsigned char *channels_p) {
	size_t bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	char type[5];
	u32_t len;

	while (bytes >= 8) {
		len = unpackN((u32_t *)streambuf->readp);
		memcpy(type, streambuf->readp + 4, 4);
		type[4] = '\0';

		// count trak to find the first playable one
		static unsigned trak, play;

		if (!strcmp(type, "moov")) {
			trak = 0;
			play = 0;
		}
		if (!strcmp(type, "trak")) {
			trak++;
		}

		// extract audio config from within esds and pass to DecInit2
		if (!strcmp(type, "esds") && bytes > len) {
			u8_t *ptr = streambuf->readp + 12;
			if (*ptr++ == 0x03) {
				mp4_desc_length(&ptr);
				ptr += 4;
			} else {
				ptr += 3;
			}
			mp4_desc_length(&ptr);
			ptr += 13;
			if (*ptr++ != 0x05) {
				LOG_WARN("error parsing esds");
				return -1;
			}
			unsigned config_len = mp4_desc_length(&ptr);
			if (a->NeAACDecInit2(a->hAac, ptr, config_len, samplerate_p, channels_p) == 0) {
				LOG_DEBUG("playable aac track: %u", trak);
				play = trak;
			}
		}

		// stash sample to chunk info, assume it comes before stco
		if (!strcmp(type, "stsc") && bytes > len && !a->chunkinfo) {
			a->stsc = malloc(len - 12);
			if (a->stsc == NULL) {
				LOG_WARN("malloc fail");
				return -1;
			}
			memcpy(a->stsc, streambuf->readp + 12, len - 12);
		}

		// build offsets table from stco and stored stsc
		if (!strcmp(type, "stco") && bytes > len && play == trak) {
			// extract chunk offsets
			u8_t *ptr = streambuf->readp + 12;
			u32_t entries = unpackN((u32_t *)ptr);
			ptr += 4;
			a->chunkinfo = malloc(sizeof(struct chunk_table) * (entries + 1));
			if (a->chunkinfo == NULL) {
				LOG_WARN("malloc fail");
				return -1;
			}
			u32_t i;
			for (i = 0; i < entries; ++i) {
				a->chunkinfo[i].offset = unpackN((u32_t *)ptr);
				a->chunkinfo[i].sample = 0;
				ptr += 4;
			}
			a->chunkinfo[i].sample = 0;
			a->chunkinfo[i].offset = 0;
			// fill in first sample id for each chunk from stored stsc
			if (a->stsc) {
				u32_t stsc_entries = unpackN((u32_t *)a->stsc);
				u32_t sample = 0;
				u32_t last = 0, last_samples = 0;
				void *ptr = a->stsc + 4;
				while (stsc_entries--) {
					u32_t first = unpackN((u32_t *)ptr);
					u32_t samples = unpackN((u32_t *)(ptr + 4));
					if (last) {
						for (i = last - 1; i < first - 1; ++i) {
							a->chunkinfo[i].sample = sample;
							sample += last_samples;
						}
					}
					if (stsc_entries == 0) {
						for (i = first - 1; i < entries; ++i) {
							a->chunkinfo[i].sample = sample;
							sample += samples;
						}
					}
					last = first;
					last_samples = samples;
					ptr += 12;
				}
				free(a->stsc);
				a->stsc = NULL;
			}
		}

		// found media data, advance to start of first chunk and return
		if (!strcmp(type, "mdat")) {
 			_buf_inc_readp(streambuf, 8);
			a->pos += 8;
			bytes  -= 8;
			if (play) {
				LOG_DEBUG("type: mdat len: %u pos: %u", len, a->pos);
				if (a->chunkinfo && a->chunkinfo[0].offset > a->pos) {
					u32_t skip = a->chunkinfo[0].offset - a->pos; 	
					LOG_DEBUG("skipping: %u", skip);
					if (skip <= bytes) {
						_buf_inc_readp(streambuf, skip);
						a->pos += skip;
					} else {
						a->consume = skip;
					}
				}
				a->sample = a->nextchunk = 1;
				return 1;
			} else {
				LOG_DEBUG("type: mdat len: %u, no playable track found", len);
				return -1;
			}
		}

		// default to consuming entire box
		u32_t consume = len;

		// read into these boxes so reduce consume
		if (!strcmp(type, "moov") || !strcmp(type, "trak") || !strcmp(type, "mdia") || !strcmp(type, "minf") || !strcmp(type, "stbl")) {
			consume = 8;
		}
		if (!strcmp(type, "stsd")) consume = 16;
		if (!strcmp(type, "mp4a")) consume = 36;

		// consume rest of box if it has been parsed (all in the buffer) or is not one we want to parse
		if (bytes >= consume) {
			LOG_DEBUG("type: %s len: %u consume: %u", type, len, consume);
			_buf_inc_readp(streambuf, consume);
			a->pos += consume;
			bytes -= consume;
		} else if (!(!strcmp(type, "esds") || !strcmp(type, "stsc") || !strcmp(type, "stco"))) {
			LOG_DEBUG("type: %s len: %u consume: %u - partial consume: %u", type, len, consume, bytes);
			_buf_inc_readp(streambuf, bytes);
			a->pos += bytes;
			a->consume = consume - bytes;
			break;
		} else {
			break;
		}
	}

	return 0;
}

static void faad_decode(void) {
	LOCK_S;
	size_t bytes_total = _buf_used(streambuf);
	size_t bytes_wrap  = min(bytes_total, _buf_cont_read(streambuf));

	if (a->consume) {
		u32_t consume = min(a->consume, bytes_wrap);
		LOG_DEBUG("consume: %u of %u", consume, a->consume);
		_buf_inc_readp(streambuf, consume);
		a->pos += consume;
		a->consume -= consume;
		UNLOCK_S;
		return;
	}

	if (decode.new_stream) {
		int found = 0;
		static unsigned char channels;
		static unsigned long samplerate;

		if (a->type == '2') {

			// adts stream - seek for header
			while (bytes_wrap >= 2 && (*(streambuf->readp) != 0xFF || (*(streambuf->readp + 1) & 0xF6) != 0xF0)) {
				_buf_inc_readp(streambuf, 1);
				bytes_total--;
				bytes_wrap--;
			}
			
			if (bytes_wrap >= 2) {
				long n = a->NeAACDecInit(a->hAac, streambuf->readp, bytes_wrap, &samplerate, &channels);
				if (n < 0) {
					found = -1;
				} else {
					_buf_inc_readp(streambuf, n);
					found = 1;
				}
			}

		} else {

			// mp4 - read header
			found = read_mp4_header(&samplerate, &channels);
		}

		if (found == 1) {

			LOG_INFO("samplerate: %u channels: %u", samplerate, channels);
			bytes_total = _buf_used(streambuf);
			bytes_wrap  = min(bytes_total, _buf_cont_read(streambuf));

			LOCK_O;
			LOG_INFO("setting track_start");
			output.next_sample_rate = samplerate; 
			output.track_start = outputbuf->writep;
			decode.new_stream = false;
			UNLOCK_O;

		} else if (found == -1) {

			LOG_WARN("error reading stream header");
			UNLOCK_S;
			LOCK_O;
			decode.state = DECODE_ERROR;
			UNLOCK_O;
			return;

		} else {

			// not finished header parsing come back next time
			UNLOCK_S;
			return;
		}
	}

	NeAACDecFrameInfo info;
	s32_t *iptr;

	if (bytes_wrap < WRAPBUF_LEN && bytes_total > WRAPBUF_LEN) {

		// make a local copy of frames which may have wrapped round the end of streambuf
		u8_t buf[WRAPBUF_LEN];
		memcpy(buf, streambuf->readp, bytes_wrap);
		memcpy(buf + bytes_wrap, streambuf->buf, WRAPBUF_LEN - bytes_wrap);

		iptr = a->NeAACDecDecode(a->hAac, &info, buf, WRAPBUF_LEN);

	} else {

		iptr = a->NeAACDecDecode(a->hAac, &info, streambuf->readp, bytes_wrap);
	}

	if (info.error) {
		LOG_WARN("error: %u %s", info.error, a->NeAACDecGetErrorMessage(info.error));
	}

	bool endstream = false;

	// mp4 end of chunk - skip to next offset
	if (a->chunkinfo && a->chunkinfo[a->nextchunk].offset && a->sample++ == a->chunkinfo[a->nextchunk].sample) {

		if (a->chunkinfo[a->nextchunk].offset > a->pos) {
			u32_t skip = a->chunkinfo[a->nextchunk].offset - a->pos;
			if (skip != info.bytesconsumed) {
				LOG_DEBUG("skipping to next chunk pos: %u consumed: %u != skip: %u", a->pos, info.bytesconsumed, skip);
			}
			if (bytes_total >= skip) {
				_buf_inc_readp(streambuf, skip);
				a->pos += skip;
			} else {
				a->consume = skip;
			}
			a->nextchunk++;
		} else {
			LOG_ERROR("error: need to skip backwards!");
			endstream = true;
		}

	// adts and mp4 when not at end of chunk 
	} else if (info.bytesconsumed != 0) {

		_buf_inc_readp(streambuf, info.bytesconsumed);
		a->pos += info.bytesconsumed;

	// error which doesn't advance streambuf - end
	} else {
		endstream = true;
	}

	UNLOCK_S;

	if (endstream) {
		LOG_WARN("unable to decode further");
		LOCK_O;
		decode.state = DECODE_ERROR;
		UNLOCK_O;
		return;
	}

	if (!info.samples) {
		return;
	}

	LOCK_O;

	size_t frames = info.samples / info.channels;
	while (frames > 0) {
		frames_t f = _buf_cont_write(outputbuf) / BYTES_PER_FRAME;
		f = min(f, frames);

		frames_t count = f;
		s32_t *optr = (s32_t *)outputbuf->writep;

		if (info.channels == 2) {
			while (count--) {
				*optr++ = *iptr++ << 8;
				*optr++ = *iptr++ << 8;
			}
		} else if (info.channels == 1) {
			while (count--) {
				*optr++ = *iptr << 8;
				*optr++ = *iptr++ << 8;
			}
		} else {
			LOG_WARN("unsupported number of channels");
		}

		frames -= f;
		_buf_inc_writep(outputbuf, f * BYTES_PER_FRAME);
	}

	UNLOCK_O;

	LOG_SDEBUG("wrote %u frames", info.samples / info.channels);
}

static void faad_open(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	LOG_INFO("opening %s stream", size == '2' ? "adts" : "mp4");

	a->type = size;
	a->pos = a->consume = a->sample = a->nextchunk = 0;

	if (a->chunkinfo) {
		free(a->chunkinfo);
	}
	if (a->stsc) {
		free(a->stsc);
	}
	a->chunkinfo = NULL;
	a->stsc = NULL;

	if (a->hAac) {
		a->NeAACDecClose(a->hAac);
	}
	a->hAac = a->NeAACDecOpen();

	NeAACDecConfigurationPtr conf = a->NeAACDecGetCurrentConfiguration(a->hAac);

	conf->outputFormat = FAAD_FMT_24BIT;
	conf->downMatrix = 1;

	if (!a->NeAACDecSetConfiguration(a->hAac, conf)) {
		LOG_WARN("error setting config");
	};
}

static void faad_close(void) {
	a->NeAACDecClose(a->hAac);
	a->hAac = NULL;
	if (a->chunkinfo) {
		free(a->chunkinfo);
		a->chunkinfo = NULL;
	}
	if (a->stsc) {
		free(a->stsc);
		a->stsc = NULL;
	}
}

static bool load_faad() {
	void *handle = dlopen(LIBFAAD, RTLD_NOW);
	if (!handle) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	a = malloc(sizeof(struct faad));

	a->hAac = NULL;
	a->chunkinfo = NULL;
	a->stsc = NULL;
	a->NeAACDecGetCapabilities = dlsym(handle, "NeAACDecGetCapabilities");
	a->NeAACDecGetCurrentConfiguration = dlsym(handle, "NeAACDecGetCurrentConfiguration");
	a->NeAACDecSetConfiguration = dlsym(handle, "NeAACDecSetConfiguration");
	a->NeAACDecOpen = dlsym(handle, "NeAACDecOpen");
	a->NeAACDecClose = dlsym(handle, "NeAACDecClose");
	a->NeAACDecInit = dlsym(handle, "NeAACDecInit");
	a->NeAACDecInit2 = dlsym(handle, "NeAACDecInit2");
	a->NeAACDecDecode = dlsym(handle, "NeAACDecDecode");
	a->NeAACDecGetErrorMessage = dlsym(handle, "NeAACDecGetErrorMessage");

	char *err;
	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);		
		return false;
	}

	LOG_INFO("loaded "LIBFAAD" cap: 0x%x", a->NeAACDecGetCapabilities());
	return true;
}

struct codec *register_faad(void) {
	static struct codec ret = { 
		.id    = 'a',
		.types = "aac",
		.open  = faad_open,
		.close = faad_close,
		.decode= faad_decode,
		.min_space = 20480,
		.min_read_bytes = WRAPBUF_LEN,
	};

	if (!load_faad()) {
		return NULL;
	}

	return &ret;
}
