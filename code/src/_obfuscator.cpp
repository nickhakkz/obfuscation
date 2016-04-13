#include <Python.h>
#include "pyutils.h"
#include "utils.h"
#include "thpool.h"
#include "thpool_fns.h"

#include <aesrand.h>
#include <clt13.h>
#include <gghlite.h>
#include <gghlite/gghlite-internals.h>
#include <omp.h>

#define debug_printf printf
#define CHECK(x, y) if((x) < (y)) { debug_printf( \
      "ERROR: fscanf() error encountered when trying to read from file\n" \
    ); }

struct state {
    threadpool thpool;
    unsigned long secparam;
    enum mmap_e mmap;
    union {
        clt_state mlm_clt;
        gghlite_sk_t mlm_gghlite;
    };
    aes_randstate_t rand;
    char *dir;
};

static void
state_destructor(PyObject *self)
{
    struct state *s;

    s = (struct state *) PyCapsule_GetPointer(self, NULL);
    if (s) {
        switch (s->mmap) {
        case MMAP_CLT:
            clt_state_clear(&s->mlm_clt);
            break;
        case MMAP_GGHLITE:
            break;
        }
        aes_randclear(s->rand);
        thpool_destroy(s->thpool);
    }
    free(s);
}

static void fread_gghlite_params(FILE *fp, gghlite_params_t params) {
  int mpfr_base = 10;
  size_t lambda, kappa, gamma, n, ell;
  uint64_t rerand_mask;
  int gghlite_flag_int;
  CHECK(fscanf(fp, "%zd %zd %zd %ld %ld %lu %d\n",
    &lambda,
    &gamma,
    &kappa,
    &n,
    &ell,
    &rerand_mask,
    &gghlite_flag_int
  ), 7);

  gghlite_params_initzero(params, lambda, kappa, gamma);
  params->n = n;
  params->ell = ell;
  params->rerand_mask = rerand_mask;
  params->flags = (gghlite_flag_t) gghlite_flag_int;

  fmpz_inp_raw(params->q, fp);
  CHECK(fscanf(fp, "\n"), 0);
  mpfr_inp_str(params->sigma, fp, mpfr_base, MPFR_RNDN);
  CHECK(fscanf(fp, "\n"), 0);
  mpfr_inp_str(params->sigma_p, fp, mpfr_base, MPFR_RNDN);
  CHECK(fscanf(fp, "\n"), 0);
  mpfr_inp_str(params->sigma_s, fp, mpfr_base, MPFR_RNDN);
  CHECK(fscanf(fp, "\n"), 0);
  mpfr_inp_str(params->ell_b, fp, mpfr_base, MPFR_RNDN);
  CHECK(fscanf(fp, "\n"), 0);
  mpfr_inp_str(params->ell_g, fp, mpfr_base, MPFR_RNDN);
  CHECK(fscanf(fp, "\n"), 0);
  mpfr_inp_str(params->xi, fp, mpfr_base, MPFR_RNDN);
  CHECK(fscanf(fp, "\n"), 0);

  fmpz_mod_poly_fread_raw(fp, params->pzt);
  CHECK(fscanf(fp, "\n"), 0);
  CHECK(fscanf(fp, "%zd\n", &params->ntt->n), 1);
  fmpz_mod_poly_fread_raw(fp, params->ntt->w);
  CHECK(fscanf(fp, "\n"), 0);
  fmpz_mod_poly_fread_raw(fp, params->ntt->w_inv);
  CHECK(fscanf(fp, "\n"), 0);
  fmpz_mod_poly_fread_raw(fp, params->ntt->phi);
  CHECK(fscanf(fp, "\n"), 0);
  fmpz_mod_poly_fread_raw(fp, params->ntt->phi_inv);

  gghlite_params_set_D_sigmas(params);
}

static void
fwrite_gghlite_params(FILE *fp, const gghlite_params_t params)
{
    int mpfr_base = 10;
    fprintf(fp, "%zd %zd %zd %ld %ld %lu %d\n",
            params->lambda,
            params->gamma,
            params->kappa,
            params->n,
            params->ell,
            params->rerand_mask,
            params->flags
        );
    fmpz_out_raw(fp, params->q);
    fprintf(fp, "\n");
    mpfr_out_str(fp, mpfr_base, 0, params->sigma, MPFR_RNDN);
    fprintf(fp, "\n");
    mpfr_out_str(fp, mpfr_base, 0, params->sigma_p, MPFR_RNDN);
    fprintf(fp, "\n");
    mpfr_out_str(fp, mpfr_base, 0, params->sigma_s, MPFR_RNDN);
    fprintf(fp, "\n");
    mpfr_out_str(fp, mpfr_base, 0, params->ell_b, MPFR_RNDN);
    fprintf(fp, "\n");
    mpfr_out_str(fp, mpfr_base, 0, params->ell_g, MPFR_RNDN);
    fprintf(fp, "\n");
    mpfr_out_str(fp, mpfr_base, 0, params->xi, MPFR_RNDN);
    fprintf(fp, "\n");
    fmpz_mod_poly_fprint_raw(fp, params->pzt);
    fprintf(fp, "\n");
    fprintf(fp, "%zd\n", params->ntt->n);
    fmpz_mod_poly_fprint_raw(fp, params->ntt->w);
    fprintf(fp, "\n");
    fmpz_mod_poly_fprint_raw(fp, params->ntt->w_inv);
    fprintf(fp, "\n");
    fmpz_mod_poly_fprint_raw(fp, params->ntt->phi);
    fprintf(fp, "\n");
    fmpz_mod_poly_fprint_raw(fp, params->ntt->phi_inv);
}


//
//
// Python functions
//
//

static PyObject *
obf_setup(PyObject *self, PyObject *args)
{
    long kappa, size, nzs, nthreads, ncores;
    struct state *s = NULL;
    PyObject *py_primes, *py_state;

    s = (struct state *) malloc(sizeof(struct state));
    if (s == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "memory allocation failed");
        return NULL;
    }
    if (!PyArg_ParseTuple(args, "llllslll", &s->secparam, &kappa, &size,
                          &nzs, &s->dir, &s->mmap, &nthreads, &ncores)) {
        goto error;
    }

    if (kappa <= 0 || size < 0 || nzs <= 0) {
        PyErr_SetString(PyExc_RuntimeError, "invalid input");
        return NULL;
    }

    if (s->mmap != MMAP_CLT && s->mmap != MMAP_GGHLITE) {
        PyErr_SetString(PyExc_RuntimeError, "invalid mmap setting");
        return NULL;
    }

    (void) aes_randinit(s->rand);
    s->thpool = thpool_init(nthreads);
    (void) omp_set_num_threads(ncores);

    if (g_verbose) {
        fprintf(stderr, "  # Threads: %ld\n", nthreads);
        fprintf(stderr, "  # Cores: %ld\n", ncores);
    }

    // Needed for AGIS obfuscator
    if (size > 0) {
        char *fname;
        fmpz_t tmp;
        int len;

        fmpz_init(tmp);
        fmpz_set_ui(tmp, size);
        len = strlen(s->dir) + 6;
        fname = (char *) calloc(len, sizeof(char));
        (void) snprintf(fname, len, "%s/size", s->dir);
        (void) save_fmpz_scalar(fname, tmp);
        free(fname);
        fmpz_clear(tmp);
    }

    switch (s->mmap) {
    case MMAP_CLT:
    {
        int *pows;
        int flags = CLT_FLAG_DEFAULT | CLT_FLAG_OPT_PARALLEL_ENCODE;

        pows = (int *) calloc(nzs, sizeof(int));
        if (pows == NULL) {
            PyErr_SetString(PyExc_RuntimeError, "memory allocation failed");
            goto error;
        }
        for (long i = 0; i < nzs; ++i) {
            pows[i] = 1;
        }

        if (g_verbose)
            flags |= CLT_FLAG_VERBOSE;
        clt_state_init(&s->mlm_clt, kappa, s->secparam, nzs, pows, flags,
                       s->rand);
        free(pows);

        // Save CLT public parameters
        {
            clt_pp pp;
            clt_pp_init(&pp, &s->mlm_clt);
            clt_pp_save(&pp, s->dir);
            clt_pp_clear(&pp);
        }
        // Convert g_i values to python objects
        //
        // Only convert the first secparam g_i values since we only need to fill
        // in the first secparam slots of the plaintext space.
        //
        py_primes = PyList_New(s->secparam);
        for (unsigned long i = 0; i < s->secparam; ++i) {
            PyList_SetItem(py_primes, i, mpz_to_py(s->mlm_clt.gs[i]));
        }
        break;
    }
    case MMAP_GGHLITE:
    {
        gghlite_flag_t flags = GGHLITE_FLAGS_DEFAULT;
        if (g_verbose)
            flags = (gghlite_flag_t) (flags | GGHLITE_FLAGS_VERBOSE);
        else
            flags = (gghlite_flag_t) (flags | GGHLITE_FLAGS_QUIET);

        gghlite_jigsaw_init_gamma(s->mlm_gghlite, s->secparam, kappa, nzs, flags, s->rand);
        if (g_verbose)
            gghlite_params_print(s->mlm_gghlite->params);

        // Save GGHLite public parameters
        {
            FILE *fp;
            char *path;
            int len = strlen(s->dir) + 10;

            path = (char *) malloc(len);
            snprintf(path, len, "%s/params", s->dir);
            fp = fopen(path, "w");
            free(path);
            if (fp == NULL) {
                PyErr_SetString(PyExc_RuntimeError, "invalid path");
                goto error;
            }
            fwrite_gghlite_params(fp, s->mlm_gghlite->params);
            (void) fclose(fp);
        }

        // Covert q to python object
        py_primes = PyList_New(1);
        PyList_SetItem(py_primes, 0, fmpz_to_py(s->mlm_gghlite->params->q));
        break;
    }
    }

    py_state = PyCapsule_New((void *) s, NULL, state_destructor);
    return PyTuple_Pack(2, py_state, py_primes);

error:
    if (s)
        free(s);

    return NULL;
}

//
// Encode N vectors across all slots of the multilinear map
//
static PyObject *
obf_encode_vectors(PyObject *self, PyObject *args)
{
    PyObject *py_state, *py_vectors;
    long index;
    char *name;
    mpz_t *vector;
    ssize_t length;
    double start;
    struct state *s;

    if (!PyArg_ParseTuple(args, "OOls", &py_state, &py_vectors, &index, &name))
        return NULL;

    s = (struct state *) PyCapsule_GetPointer(py_state, NULL);
    if (s == NULL)
        return NULL;

    start = current_time();

    // We assume that all vectors have the same length, and thus just grab the
    // length of the first vector
    length = PyList_GET_SIZE(PyList_GET_ITEM(py_vectors, 0));
    vector = (mpz_t *) calloc(length, sizeof(mpz_t));
    for (ssize_t i = 0; i < length; ++i) {
        mpz_init(vector[i]);
    }

    {
        struct write_vector_s *wv_s;

        wv_s = (struct write_vector_s *) malloc(sizeof(write_vector_s));
        wv_s->dir = (char *) calloc(strlen(s->dir) + 1, sizeof(char));
        (void) strcpy(wv_s->dir, s->dir);
        wv_s->name = (char *) calloc(strlen(name) + 1, sizeof(char));
        (void) strcpy(wv_s->name, name);
        wv_s->vector = vector;
        wv_s->length = length;
        wv_s->start = start;

        (void) thpool_add_tag(s->thpool, name, length, thpool_write_vector,
                              wv_s);
        
        for (ssize_t i = 0; i < length; ++i) {
            mpz_t *elems;
            struct encode_elem_s *args;

            elems = (mpz_t *) calloc(s->secparam, sizeof(mpz_t));
            for (unsigned long j = 0; j < s->secparam; ++j) {
                mpz_init(elems[j]);
                py_to_mpz(elems[j],
                          PyList_GET_ITEM(PyList_GET_ITEM(py_vectors, j), i));
            }
            
            args = (struct encode_elem_s *) malloc(sizeof(struct encode_elem_s));
            args->mlm = &s->mlm_clt;
            args->rand = &s->rand;
            args->out = &vector[i];
            args->nins = s->mlm_clt.nzs;
            args->ins = elems;
            args->pows = (int *) calloc(args->nins, sizeof(int));
            args->pows[index] = 1;

            // for (unsigned long i = 0; i < args->nins; ++i) {
            //     printf("%d ", args->pows[i]);
            // }
            // printf("\n");

            thpool_add_work(s->thpool, thpool_encode_elem, (void *) args, name);
        }
    }

    Py_RETURN_NONE;
}

static void
_obf_encode_layers_clt(struct state *s, long idx, long inp, long nrows,
                       long ncols, PyObject *py_zero_ms, PyObject *py_one_ms)
{
    struct write_layer_s *wl_s;
    clt_elem_t *zero, *one;
    double start;
    char idx_s[10];

    start = current_time();

    (void) snprintf(idx_s, 10, "%ld", idx);

    zero = (clt_elem_t *) malloc(sizeof(clt_elem_t) * nrows * ncols);
    one = (clt_elem_t *) malloc(sizeof(clt_elem_t) * nrows * ncols);
    for (ssize_t i = 0; i < nrows * ncols; ++i) {
        mpz_inits(zero[i], one[i], NULL);
    }

    wl_s = (struct write_layer_s *) malloc(sizeof(write_layer_s));
    wl_s->dir = (char *) calloc(strlen(s->dir) + 1, sizeof(char));
    (void) strcpy(wl_s->dir, s->dir);
    wl_s->mmap = s->mmap;
    wl_s->zero = zero;
    wl_s->one = one;
    wl_s->inp = inp;
    wl_s->idx = idx;
    wl_s->nrows = nrows;
    wl_s->ncols = ncols;
    wl_s->start = start;

    (void) thpool_add_tag(s->thpool, idx_s, 2 * nrows * ncols,
                          thpool_write_layer, wl_s);

    for (Py_ssize_t ctr = 0; ctr < 2 * nrows * ncols; ++ctr) {
        PyObject *py_array;
        clt_elem_t *val;
        size_t i;
        clt_elem_t *elems;
        struct encode_elem_s *args;

        if (ctr < nrows * ncols) {
            i = ctr;
            val = &zero[i];
            py_array = py_zero_ms;
        } else {
            i = ctr - nrows * ncols;
            val = &one[i];
            py_array = py_one_ms;
        }

        elems = (clt_elem_t *) malloc(sizeof(clt_elem_t) * s->secparam);
        for (unsigned long j = 0; j < s->secparam; ++j) {
            mpz_init(elems[j]);
            py_to_mpz(elems[j],
                      PyList_GET_ITEM(PyList_GET_ITEM(py_array, j), i));
        }

        args = (struct encode_elem_s *) malloc(sizeof(struct encode_elem_s));
        args->mmap = s->mmap;
        args->mlm = &s->mlm_clt;
        args->rand = NULL;
        args->out = val;
        args->nins = s->mlm_clt.nzs;
        args->ins = elems;
        args->pows = (int *) calloc(args->nins, sizeof(int));
        args->pows[idx] = 1;

        // for (unsigned long i = 0; i < args->nins; ++i) {
        //     printf("%d ", args->pows[i]);
        // }
        // printf("\n");

        thpool_add_work(s->thpool, thpool_encode_elem, (void *) args, idx_s);
    }
}

static void
_obf_encode_layers_gghlite(struct state *s, long idx, long inp, long nrows,
                           long ncols, PyObject *py_zero_ms, PyObject *py_one_ms)
{
    struct write_layer_s *wl_s;
    gghlite_enc_t *zero, *one;
    double start;
    char idx_s[10];

    start = current_time();

    (void) snprintf(idx_s, 10, "%ld", idx);

    zero = (gghlite_enc_t *) malloc(sizeof(gghlite_enc_t) * nrows * ncols);
    one = (gghlite_enc_t *) malloc(sizeof(gghlite_enc_t) * nrows * ncols);
    for (ssize_t i = 0; i < nrows * ncols; ++i) {
        gghlite_enc_init(zero[i], s->mlm_gghlite->params);
        gghlite_enc_init(one[i], s->mlm_gghlite->params);
    }

    wl_s = (struct write_layer_s *) malloc(sizeof(write_layer_s));
    wl_s->dir = (char *) calloc(strlen(s->dir) + 1, sizeof(char));
    (void) strcpy(wl_s->dir, s->dir);
    wl_s->mmap = s->mmap;
    wl_s->zero = zero;
    wl_s->one = one;
    wl_s->inp = inp;
    wl_s->idx = idx;
    wl_s->nrows = nrows;
    wl_s->ncols = ncols;
    wl_s->start = start;

    (void) thpool_add_tag(s->thpool, idx_s, 2 * nrows * ncols,
                          thpool_write_layer, wl_s);

    for (Py_ssize_t ctr = 0; ctr < 2 * nrows * ncols; ++ctr) {
        PyObject *py_array;
        gghlite_enc_t *val;
        size_t i;
        fmpz_t *elem;
        struct encode_elem_s *args;

        elem = (fmpz_t *) malloc(sizeof(fmpz_t));
        fmpz_init(*elem);

        if (ctr < nrows * ncols) {
            i = ctr;
            val = &zero[i];
            py_array = py_zero_ms;
        } else {
            i = ctr - nrows * ncols;
            val = &one[i];
            py_array = py_one_ms;
        }

        py_to_fmpz(*elem, PyList_GET_ITEM(PyList_GET_ITEM(py_array, 0), i));

        args = (struct encode_elem_s *) malloc(sizeof(struct encode_elem_s));
        args->mmap = s->mmap;
        args->mlm = &s->mlm_gghlite;
        args->rand = NULL;
        args->out = val;
        args->nins = 1;
        args->ins = elem;
        args->pows = (int *) calloc(s->mlm_gghlite->params->gamma, sizeof(int));

        thpool_add_work(s->thpool, thpool_encode_elem, (void *) args, idx_s);
    }
}

//
// Encode layers across all slots of the multilinear map
//
static PyObject *
obf_encode_layers(PyObject *self, PyObject *args)
{
    PyObject *py_zero_ms, *py_one_ms;
    PyObject *py_state;
    long inp, idx, nrows, ncols;

    struct state *s;

    if (!PyArg_ParseTuple(args, "OllllOO", &py_state, &idx, &nrows, &ncols,
                          &inp, &py_zero_ms, &py_one_ms))
        return NULL;

    s = (struct state *) PyCapsule_GetPointer(py_state, NULL);
    if (s == NULL)
        return NULL;

    switch (s->mmap) {
    case MMAP_CLT:
        _obf_encode_layers_clt(s, idx, inp, nrows, ncols, py_zero_ms,
                               py_one_ms);
        break;
    case MMAP_GGHLITE:
        _obf_encode_layers_gghlite(s, idx, inp, nrows, ncols, py_zero_ms,
                                   py_one_ms);
        break;
    }

    Py_RETURN_NONE;
}

static int
_obf_sz_evaluate_clt(clt_pp *pp, char *dir, char *input, long bplen)
{
    char *fname = NULL;
    int fnamelen;
    mpz_t tmp, *result = NULL;
    long nrows, ncols = -1, nrows_prev = -1;
    int err = 0, iszero = -1;
    double start, end;

    fnamelen = strlen(dir) + sizeof bplen + 7;
    fname = (char *) malloc(sizeof(char) * fnamelen);

    mpz_inits(tmp, NULL);

    for (int layer = 0; layer < bplen; ++layer) {
        unsigned int input_idx;
        mpz_t *left, *right;

        start = current_time();

        // determine the size of the matrix
        (void) snprintf(fname, fnamelen, "%s/%d.nrows", dir, layer);
        (void) load_mpz_scalar(fname, tmp);
        nrows = mpz_get_ui(tmp);
        (void) snprintf(fname, fnamelen, "%s/%d.ncols", dir, layer);
        (void) load_mpz_scalar(fname, tmp);
        ncols = mpz_get_ui(tmp);

        // find out the input bit for the given layer
        (void) snprintf(fname, fnamelen, "%s/%d.input", dir, layer);
        (void) load_mpz_scalar(fname, tmp);
        input_idx = mpz_get_ui(tmp);
        if (input_idx >= strlen(input)) {
            PyErr_SetString(PyExc_RuntimeError, "invalid input");
            err = 1;
            break;
        }
        if (input[input_idx] != '0' && input[input_idx] != '1') {
            PyErr_SetString(PyExc_RuntimeError, "input must be 0 or 1");
            err = 1;
            break;
        }
        // load in appropriate matrix for the given input value
        if (input[input_idx] == '0') {
            (void) snprintf(fname, fnamelen, "%s/%d.zero", dir, layer);
        } else {
            (void) snprintf(fname, fnamelen, "%s/%d.one", dir, layer);
        }

        if (layer == 0) {
            result = (mpz_t *) malloc(sizeof(mpz_t) * nrows * ncols);
            for (int i = 0; i < nrows * ncols; ++i) {
                mpz_init(result[i]);
            }
            (void) load_mpz_vector(fname, result, nrows * ncols);
            nrows_prev = nrows;
        } else {
            left = result;
            right = (mpz_t *) malloc(sizeof(mpz_t) * nrows * ncols);
            for (int i = 0; i < nrows * ncols; ++i) {
                mpz_init(right[i]);
            }
            (void) load_mpz_vector(fname, right, nrows * ncols);
            result = (mpz_t *) malloc(sizeof(mpz_t) * nrows_prev * ncols);
            for (int i = 0; i < nrows_prev * ncols; ++i) {
                mpz_init(result[i]);
            }
            mult_mpz_matrices(result, left, right, pp->x0, nrows_prev, nrows,
                              ncols);

            for (int i = 0; i < nrows_prev * nrows; ++i) {
                mpz_clear(left[i]);
            }
            for (int i = 0; i < nrows * ncols; ++i) {
                mpz_clear(right[i]);
            }
            free(left);
            free(right);
        }
        end = current_time();

        if (g_verbose)
            (void) fprintf(stderr, "  Multiplying matrices: %f\n",
                           end - start);
    }

    if (!err) {
        start = current_time();
        iszero = clt_is_zero(pp, result[1]);
        end = current_time();
        if (g_verbose)
            (void) fprintf(stderr, "  Zero test: %f\n", end - start);
    }

    for (int i = 0; i < nrows_prev * ncols; ++i) {
        mpz_clear(result[i]);
    }
    free(result);

    mpz_clears(tmp, NULL);

    return iszero;
}

static int
_obf_sz_evaluate_gghlite(gghlite_params_t pp, char *dir, char *input, long bplen)
{
    char *fname = NULL;
    int fnamelen;
    mpz_t tmp;
    gghlite_enc_t *result = NULL;
    long nrows, ncols = -1, nrows_prev = -1;
    int err = 0, iszero = -1;
    double start, end;

    fnamelen = strlen(dir) + sizeof bplen + 7;
    fname = (char *) malloc(sizeof(char) * fnamelen);

    mpz_inits(tmp, NULL);

    for (int layer = 0; layer < bplen; ++layer) {
        unsigned int input_idx;
        gghlite_enc_t *left, *right;

        start = current_time();

        // determine the size of the matrix
        (void) snprintf(fname, fnamelen, "%s/%d.nrows", dir, layer);
        (void) load_mpz_scalar(fname, tmp);
        nrows = mpz_get_ui(tmp);
        (void) snprintf(fname, fnamelen, "%s/%d.ncols", dir, layer);
        (void) load_mpz_scalar(fname, tmp);
        ncols = mpz_get_ui(tmp);

        // find out the input bit for the given layer
        (void) snprintf(fname, fnamelen, "%s/%d.input", dir, layer);
        (void) load_mpz_scalar(fname, tmp);
        input_idx = mpz_get_ui(tmp);
        if (input_idx >= strlen(input)) {
            PyErr_SetString(PyExc_RuntimeError, "invalid input");
            err = 1;
            break;
        }
        if (input[input_idx] != '0' && input[input_idx] != '1') {
            PyErr_SetString(PyExc_RuntimeError, "input must be 0 or 1");
            err = 1;
            break;
        }
        // load in appropriate matrix for the given input value
        if (input[input_idx] == '0') {
            (void) snprintf(fname, fnamelen, "%s/%d.zero", dir, layer);
        } else {
            (void) snprintf(fname, fnamelen, "%s/%d.one", dir, layer);
        }

        if (layer == 0) {
            result = (gghlite_enc_t *) malloc(sizeof(gghlite_enc_t) * nrows * ncols);
            for (int i = 0; i < nrows * ncols; ++i) {
                gghlite_enc_init(result[i], pp);
            }
            (void) load_gghlite_enc_vector(fname, result, nrows * ncols);
            nrows_prev = nrows;
        } else {
            left = result;
            right = (gghlite_enc_t *) malloc(sizeof(gghlite_enc_t) * nrows * ncols);
            for (int i = 0; i < nrows * ncols; ++i) {
                gghlite_enc_init(right[i], pp);
            }
            (void) load_gghlite_enc_vector(fname, right, nrows * ncols);
            result = (gghlite_enc_t *) malloc(sizeof(gghlite_enc_t) * nrows_prev * ncols);
            for (int i = 0; i < nrows_prev * ncols; ++i) {
                gghlite_enc_init(result[i], pp);
            }
            mult_gghlite_enc_matrices(result, pp, left, right, pp->q,
                                      nrows_prev, nrows, ncols);

            for (int i = 0; i < nrows_prev * nrows; ++i) {
                gghlite_enc_clear(left[i]);
            }
            for (int i = 0; i < nrows * ncols; ++i) {
                gghlite_enc_clear(right[i]);
            }
            free(left);
            free(right);
        }
        end = current_time();

        if (g_verbose)
            (void) fprintf(stderr, "  Multiplying matrices: %f\n", end - start);
    }

    if (!err) {
        start = current_time();
        iszero = gghlite_enc_is_zero(pp, result[1]);
        end = current_time();
        if (g_verbose)
            (void) fprintf(stderr, "  Zero test: %f\n", end - start);
    }

    for (int i = 0; i < nrows_prev * ncols; ++i) {
        gghlite_enc_clear(result[i]);
    }
    free(result);

    mpz_clears(tmp, NULL);

    return iszero;
}

static PyObject *
obf_sz_evaluate(PyObject *self, PyObject *args)
{
    char *dir = NULL;
    char *input = NULL;
    int iszero = -1;
    enum mmap_e mmap;
    long bplen, nthreads;

    if (!PyArg_ParseTuple(args, "sslll", &dir, &input, &bplen, &mmap, &nthreads)) {
        PyErr_SetString(PyExc_RuntimeError, "error parsing arguments");
        return NULL;
    }

    if (mmap != MMAP_CLT && mmap != MMAP_GGHLITE) {
        PyErr_SetString(PyExc_RuntimeError, "invalid mmap setting");
        return NULL;
    }

    (void) omp_set_num_threads(nthreads);

    switch (mmap) {
    case MMAP_CLT:
    {
        clt_pp pp_clt;
        if (clt_pp_read(&pp_clt, dir) == 1)
            return NULL;
        iszero = _obf_sz_evaluate_clt(&pp_clt, dir, input, bplen);
        clt_pp_clear(&pp_clt);
        break;
    }
    case MMAP_GGHLITE:
    {
        FILE *fp;
        char *path;
        int len = strlen(dir) + 10;
        gghlite_params_t pp_gghlite;

        path = (char *) malloc(len);
        snprintf(path, len, "%s/params", dir);
        fp = fopen(path, "r");
        free(path);
        if (fp == NULL) {
            PyErr_SetString(PyExc_RuntimeError, "invalid path");
            return NULL;
        }
        fread_gghlite_params(fp, pp_gghlite);
        (void) fclose(fp);
        iszero = _obf_sz_evaluate_gghlite(pp_gghlite, dir, input, bplen);
        break;
    }
    }

    if (iszero == -1)
        return NULL;
    else
        return Py_BuildValue("i", iszero ? 0 : 1);
}

static PyObject *
obf_evaluate(PyObject *self, PyObject *args)
{
    char *dir = NULL;
    char *input = NULL;
    char *fname = NULL;
    int fnamelen;
    int iszero = -1;
    clt_pp pp;
    mpz_t *comp, *s, *t;
    mpz_t tmp;
    long bplen, size, nthreads;
    int err = 0;
    enum mmap_e mmap;
    double start, end;

    if (!PyArg_ParseTuple(args, "sslll", &dir, &input, &bplen, &mmap, &nthreads))
        return NULL;
    fnamelen = strlen(dir) + sizeof bplen + 7;
    fname = (char *) malloc(sizeof(char) * fnamelen);
    if (fname == NULL)
        return NULL;

    mpz_inits(tmp, NULL);
    clt_pp_read(&pp, dir);

    // Get the size of the matrices
    (void) snprintf(fname, fnamelen, "%s/size", dir);
    (void) load_mpz_scalar(fname, tmp);
    size = mpz_get_ui(tmp);

    comp = (mpz_t *) malloc(sizeof(mpz_t) * size * size);
    s = (mpz_t *) malloc(sizeof(mpz_t) * size);
    t = (mpz_t *) malloc(sizeof(mpz_t) * size);
    if (!comp || !s || !t) {
        err = 1;
        goto cleanup;
    }
    for (int i = 0; i < size; ++i) {
        mpz_inits(s[i], t[i], NULL);
    }
    for (int i = 0; i < size * size; ++i) {
        mpz_init(comp[i]);
    }

    (void) omp_set_num_threads(nthreads);

    for (int layer = 0; layer < bplen; ++layer) {
        unsigned int input_idx;

        start = current_time();
        // find out the input bit for the given layer
        (void) snprintf(fname, fnamelen, "%s/%d.input", dir, layer);
        (void) load_mpz_scalar(fname, tmp);
        input_idx = mpz_get_ui(tmp);
        if (input_idx >= strlen(input)) {
            PyErr_SetString(PyExc_RuntimeError, "invalid input");
            err = 1;
            break;
        }
        if (input[input_idx] != '0' && input[input_idx] != '1') {
            PyErr_SetString(PyExc_RuntimeError, "input must be 0 or 1");
            err = 1;
            break;
        }

        // load in appropriate matrix for the given input value
        if (input[input_idx] == '0') {
            (void) snprintf(fname, fnamelen, "%s/%d.zero", dir, layer);
        } else {
            (void) snprintf(fname, fnamelen, "%s/%d.one", dir, layer);
        }
        (void) load_mpz_vector(fname, comp, size * size);

        // for the first matrix, multiply 'comp' by 's' to get a vector
        if (layer == 0) {
            (void) snprintf(fname, fnamelen, "%s/s_enc", dir);
            (void) load_mpz_vector(fname, s, size);
        }
        mult_vect_by_mat(s, comp, pp.x0, size, t);
        end = current_time();
        if (g_verbose)
            (void) fprintf(stderr, " Multiplying matrices: %f\n",
                           end - start);
    }

    if (!err) {
        start = current_time();
        (void) snprintf(fname, fnamelen, "%s/t_enc", dir);
        (void) load_mpz_vector(fname, t, size);
        mult_vect_by_vect(tmp, s, t, pp.x0, size);
        end = current_time();
        if (g_verbose)
            (void) fprintf(stderr, " Multiplying vectors: %f\n",
                           end - start);

        start = current_time();
        {
            iszero = clt_is_zero(&pp, tmp);
        }
        end = current_time();
        if (g_verbose)
            (void) fprintf(stderr, " Zero test: %f\n", end - start);
    }

    for (int i = 0; i < size; ++i) {
        mpz_clears(s[i], t[i], NULL);
    }
    for (int i = 0; i < size * size; ++i) {
        mpz_clear(comp[i]);
    }
cleanup:
    clt_pp_clear(&pp);
    mpz_clears(tmp, NULL);
    if (comp)
        free(comp);
    if (s)
        free(s);
    if (t)
        free(t);
    if (fname)
        free(fname);
    if (err)
        return NULL;
    else
        return Py_BuildValue("i", iszero ? 0 : 1);
}

static PyObject *
obf_wait(PyObject *self, PyObject *args)
{
    PyObject *py_state;
    struct state *s;

    if (!PyArg_ParseTuple(args, "O", &py_state))
        return NULL;

    s = (struct state *) PyCapsule_GetPointer(py_state, NULL);
    if (s == NULL)
        return NULL;

    thpool_wait(s->thpool);

    Py_RETURN_NONE;
}

static PyMethodDef
ObfMethods[] = {
    {"verbose", obf_verbose, METH_VARARGS,
     "Set verbosity."},
    {"setup", obf_setup, METH_VARARGS,
     "Set up obfuscator."},
    {"encode_vectors", obf_encode_vectors, METH_VARARGS,
     "Encode a vector in each slot."},
    {"encode_layers", obf_encode_layers, METH_VARARGS,
     "Encode a branching program layer in each slot."},
    {"max_mem_usage", obf_max_mem_usage, METH_VARARGS,
     "Print out the maximum memory usage."},
    {"evaluate", obf_evaluate, METH_VARARGS,
     "Evaluate the obfuscation."},
    {"sz_evaluate", obf_sz_evaluate, METH_VARARGS,
     "Evaluate the obfuscation."},
    {"wait", obf_wait, METH_VARARGS,
     "Wait for threadpool to empty."},
    {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC
init_obfuscator(void)
{
    (void) Py_InitModule("_obfuscator", ObfMethods);
}
