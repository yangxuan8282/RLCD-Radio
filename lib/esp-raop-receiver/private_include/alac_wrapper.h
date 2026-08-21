/*****************************************************************************
 * alac_wrapper.h: ALAC coder wrapper
 *
 * (c) Philippe G. 2019, philippe_44@outlook.com
 *
 * This software is released under the MIT License.
 * https://opensource.org/licenses/MIT
 *
 */

#ifndef __ALAC_WRAPPER_H_
#define __ALAC_WRAPPER_H_

struct alac_codec_s;

#ifdef __cplusplus
extern "C" {
#endif

struct alac_codec_s *alac_create_decoder(int magic_cookie_size, unsigned char *magic_cookie,
								unsigned char *sample_size, unsigned *sample_rate,
								unsigned char *channels, unsigned int *block_size);
void alac_delete_decoder(struct alac_codec_s *codec);
bool alac_to_pcm(struct alac_codec_s *codec, unsigned char* input,
				 unsigned char *output, char channels, unsigned *out_frames);

#ifdef __cplusplus
}
#endif

#endif
