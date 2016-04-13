#ifndef __OBFUSCATION__UTILS_H__
#define __OBFUSCATION__UTILS_H__

#include <gmp.h>
#include <gghlite.h>
#include <flint/fmpz.h>
#include <flint/fmpz_mod_poly.h>

extern int g_verbose;

#define OBFUSCATOR_OK 0
#define OBFUSCATOR_ERR (-1)

enum mmap_e { MMAP_CLT, MMAP_GGHLITE };

double
current_time(void);

int
seed_rng(gmp_randstate_t *rng);

int
fmpz_mod_poly_fread_raw(FILE * f, fmpz_mod_poly_t poly);
int
fmpz_mod_poly_fprint_raw(FILE * file, const fmpz_mod_poly_t poly);

int
load_mpz_scalar(const char *fname, mpz_t x);
int
save_mpz_scalar(const char *fname, const mpz_t x);

int
save_fmpz_scalar(const char *fname, const fmpz_t x);

int
load_mpz_vector(const char *fname, mpz_t *m, const int len);
int
save_mpz_vector(const char *fname, const mpz_t *m, const int len);

int
load_gghlite_enc_vector(const char *fname, gghlite_enc_t *m, const int len);
int
save_gghlite_enc_vector(const char *fname, const gghlite_enc_t *m, const int len);

void
mult_mpz_matrices(mpz_t *result, const mpz_t *left, const mpz_t *right,
                  const mpz_t q, long m, long n, long p);
void
mult_gghlite_enc_matrices(gghlite_enc_t *result, const gghlite_params_t pp,
                          const gghlite_enc_t *left, const gghlite_enc_t *right,
                          const fmpz_t q, long m, long n, long p);

void
mult_vect_by_mat(mpz_t *v, const mpz_t *m, mpz_t q, int size, mpz_t *tmparray);

void
mult_vect_by_vect(mpz_t out, const mpz_t *m, const mpz_t *v, mpz_t q, int size);

void
mpz_genrandom(mpz_t rnd, gmp_randstate_t *rng, const long nbits);

#endif
