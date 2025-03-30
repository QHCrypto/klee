#pragma once

#ifndef BONC_H
#define BONC_H

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get an input block with given @c size and @c name .
 * 
 * The input block will have name @c bonc:input:{name} as a symbol value in KLEE.
 * @param size The input block size in bytes
 * @param name The input block name.
 * @return void* 
 */
void *bonc_input(size_t size, const char *name);

/* Several convenient way for specifying input. */

void *bonc_input_plaintext(size_t size);
void *bonc_input_message(size_t size);
void *bonc_input_key(size_t size);
void *bonc_input_iv(size_t size);
void *bonc_input_nonce(size_t size);

/**
 * @brief Introduce the round number super-parameter.
 * 
 * We will spawn different process for different 
 * round number from @c 1 to @c max_round .
 * @param max_round The maximum (or designed) round number.
 * @return size_t
 */
size_t bonc_round_number(size_t max_round);

/**
 * @brief Mark @c size bytes from @c addr is the output of this algorithm.
 * 
 * @param addr 
 * @param size 
 * @param name 
 */
void bonc_output(void *addr, size_t size, const char *name);

void bonc_output_ciphertext(void *addr, size_t size);
void bonc_output_keystream(void *addr, size_t size);
void bonc_output_tag(void *addr, size_t size);


#ifdef __cplusplus
}
#endif

#endif