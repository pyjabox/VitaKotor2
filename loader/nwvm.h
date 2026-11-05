/* nwvm.h -- the NWScript interpreter in C (nwvm.c). */
#ifndef NWVM_H
#define NWVM_H

void nwvm_install(void);
void nwvm_on_swap(void);
/* NWVM_AB: which interpreter runs this window; the AI thread ends windows. */
int nwvm_phase_ours(void);
void nwvm_ab_window_end(void);
/* For the verifier (nwvm_verify.c): resolve the engine functions, then run one
 * program through ours or the engine's ExecuteCode. */
int nwvm_resolve(void);
int nwvm_run(void *vm, int *pip, char *code, int size);
int nwvm_run_engine(void *vm, int *pip, char *code, int size);

#endif
