#include "utils.h"

#include <fcntl.h>
#include <gmp.h>
#include <math.h>
#include <omp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

int g_verbose;

double
current_time(void)
{
    struct timeval t;
    (void) gettimeofday(&t, NULL);
    return (double) (t.tv_sec + (double) (t.tv_usec / 1000000.0));
}

int
fmpz_mod_poly_fread_raw(FILE * f, fmpz_mod_poly_t poly)
{
    slong i, length;
    fmpz_t coeff;
    ulong res;

    fmpz_init(coeff);
    if (flint_fscanf(f, "%wd ", &length) != 1) {
        fmpz_clear(coeff);
        return 0;
    }

    fmpz_inp_raw(coeff,f);
    fmpz_mod_poly_init(poly, coeff);
    fmpz_mod_poly_fit_length(poly, length);

    poly->length = length;
    flint_fscanf(f, " ");

    for (i = 0; i < length; i++)
    {
        flint_fscanf(f, " ");
        res = fmpz_inp_raw(coeff, f);

        fmpz_mod_poly_set_coeff_fmpz(poly,i,coeff);

        if (!res)
        {
            poly->length = i;
            fmpz_clear(coeff);
            return 0;
        }
    }

    fmpz_clear(coeff);
    _fmpz_mod_poly_normalise(poly);

    return 1;
}

static int
_fmpz_mod_poly_fprint_raw(FILE * file, const fmpz *poly, slong len, const fmpz_t p)
{
    int r;
    slong i;

    r = flint_fprintf(file, "%wd ", len);
    if (r <= 0)
        return r;

    r = fmpz_out_raw(file, p);
    if (r <= 0)
        return r;

    if (len == 0)
        return r;

    r = flint_fprintf(file, " ");
    if (r <= 0)
        return r;

    for (i = 0; (r > 0) && (i < len); i++)
    {
        r = flint_fprintf(file, " ");
        if (r <= 0)
            return r;
        r = fmpz_out_raw(file, poly + i);
        if (r <= 0)
            return r;
    }

    return r;
}

int
fmpz_mod_poly_fprint_raw(FILE * file, const fmpz_mod_poly_t poly)
{
    return _fmpz_mod_poly_fprint_raw(file, poly->coeffs, poly->length,
        &(poly->p));
}


int
load_mpz_scalar(const char *fname, mpz_t x)
{
    FILE *f;
    if ((f = fopen(fname, "r")) == NULL) {
        perror(fname);
        return 1;
    }
    (void) mpz_inp_raw(x, f);
    (void) fclose(f);
    return 0;
}

int
save_mpz_scalar(const char *fname, const mpz_t x)
{
    FILE *f;
    if ((f = fopen(fname, "w")) == NULL) {
        perror(fname);
        return 1;
    }
    if (mpz_out_raw(f, x) == 0) {
        (void) fclose(f);
        return 1;
    }
    (void) fclose(f);
    return 0;
}

int
load_gghlite_enc_vector(const char *fname, gghlite_enc_t *m, const int len)
{
    FILE *f;
    if ((f = fopen(fname, "r")) == NULL) {
        perror(fname);
        return 1;
    }
    for (int i = 0; i < len; ++i) {
        (void) fmpz_mod_poly_fread_raw(f, m[i]);
    }
    (void) fclose(f);
    return 0;
    
}
int
save_gghlite_enc_vector(const char *fname, const gghlite_enc_t *m, const int len)
{
    FILE *f;
    if ((f = fopen(fname, "w")) == NULL) {
        perror(fname);
        return 1;
    }
    for (int i = 0; i < len; ++i) {
        if (fmpz_mod_poly_fprint_raw(f, m[i]) == 0) {
            (void) fclose(f);
            return 1;
        }
    }
    (void) fclose(f);
    return 0;
}

void
mult_clt_elem_matrices(clt_elem_t *result, const clt_elem_t *left,
                       const clt_elem_t *right, const clt_elem_t q, long m,
                       long n, long p)
{
    clt_elem_t *tmparray;
    double start, end;

    start = current_time();
    tmparray = (clt_elem_t *) malloc(sizeof(clt_elem_t) * m * p);
    for (int i = 0; i < m * p; ++i) {
        clt_elem_init(tmparray[i]);
    }
#pragma omp parallel for
    for (int i = 0; i < m; ++i) {
        for (int j = 0; j < p; ++j) {
            clt_elem_t tmp, sum;
            clt_elem_init(tmp);
            clt_elem_init(sum);
            for (int k = 0; k < n; ++k) {
                clt_elem_mul(tmp,
                             left[k * m + (i * m + j) % m],
                             right[k + n * ((i * m + j) / m)]);
                clt_elem_add(sum, sum, tmp);
                clt_elem_mod(sum, sum, q);
            }
            clt_elem_set(tmparray[i * n + j], sum);
            clt_elem_clear(tmp);
            clt_elem_clear(sum);
        }
    }
    for (int i = 0; i < m * p; ++i) {
        clt_elem_set(result[i], tmparray[i]);
        clt_elem_clear(tmparray[i]);
    }
    free(tmparray);
    end = current_time();
    if (g_verbose)
        (void) fprintf(stderr, " Multiplying took: %f\n", end - start);
}

void
mult_gghlite_enc_matrices(gghlite_enc_t *result, const gghlite_params_t pp,
                          const gghlite_enc_t *left, const gghlite_enc_t *right,
                          const fmpz_t q, long m, long n, long p)
{
    gghlite_enc_t *tmparray;
    double start, end;

    start = current_time();
    tmparray = (gghlite_enc_t *) malloc(sizeof(gghlite_enc_t) * m * p);
    for (int i = 0; i < m * p; ++i) {
        gghlite_enc_init(tmparray[i], pp);
    }
#pragma omp parallel for
    for (int i = 0; i < m; ++i) {
        for (int j = 0; j < p; ++j) {
            gghlite_enc_t tmp, sum;
            gghlite_enc_init(tmp, pp);
            gghlite_enc_init(sum, pp);
            for (int k = 0; k < n; ++k) {
                gghlite_enc_mul(tmp, pp,
                                left[k * m + (i * m + j) % m],
                                right[k + n * ((i * m + j) / m)]);
                gghlite_enc_add(sum, pp, sum, tmp);
                // mpz_mod(sum, sum, q);
            }
            gghlite_enc_set(tmparray[i * n + j], sum);
            gghlite_enc_clear(tmp);
            gghlite_enc_clear(sum);
        }
    }
    for (int i = 0; i < m * p; ++i) {
        gghlite_enc_set(result[i], tmparray[i]);
        gghlite_enc_clear(tmparray[i]);
    }
    free(tmparray);
    end = current_time();
    if (g_verbose)
        (void) fprintf(stderr, " Multiplying took: %f\n", end - start);
}
