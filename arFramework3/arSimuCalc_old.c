/*
 *  MATLAB usage: arSimuCalc(struct ar, int fine, int sensi)
 *
 *  (adaptation from Scott D. Cohen, Alan C. Hindmarsh, and Radu Serban @ LLNL)
 *
 *  Copyright Andreas Raue 2011 (andreas.raue@fdm.uni-freiburg.de)
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mex.h>
#include <pthread.h>

#include <cvodes/cvodes.h>           /* prototypes for CVODES fcts. and consts. */
#include <cvodes/cvodes_dense.h>     /* prototype for CVDENSE fcts. and constants */
#include <nvector/nvector_serial.h>  /* defs. of serial NVECTOR fcts. and macros  */
#include <sundials/sundials_types.h> /* def. of type realtype */
#include <sundials/sundials_math.h>  /* definition of ABS */

/* Accessor macros */
#define Ith(v, i)    NV_Ith_S(v, i-1)       /* i-th vector component i=1..neq */
#define IJth(A, i, j) DENSE_ELEM(A, i-1, j-1) /* (i,j)-th matrix component i,j=1..neq */

/* user variables */
#include "arSimuCalcVariables.c"

struct thread_data_x {
    int	im;
    int ic;
    mxArray *arcondition;
};

struct thread_data_y {
    int	im;
    int id;
    mxArray *ardata;
    mxArray *arcondition;
    int dt;
};

typedef struct {
    double *u;
    double *su;
    double *p;
    double *v;
    double *dvdx;
    double *dvdu;
    double *dvdp;
    double *sv;
} *UserData;

mxArray *armodel;

int  fine;
int  sensi; 
int  jacobian;
int  parallel;
double  cvodes_rtol;
double  cvodes_atol;
int  cvodes_maxsteps;
int  fiterrors;
double fiterrors_correction;
bool error_corr = FALSE;

/* Prototypes of private functions */
void *x_calc(void *threadarg);
void *y_calc(void *threadarg);

static void fres(int nt, int ny, int it, double *res, double *y, double *yexp, double *ystd, double *chi2);
static void fsres(int nt, int ny, int np, int it, double *sres, double *sy, double *yexp, double *ystd);
static void fres_error(int nt, int ny, int it, double *reserr, double *res, double *y, double *yexp, double *ystd, double *chi2);
static void fsres_error(int nt, int ny, int np, int it, double *sres, double *sreserr, double *sy, double *systd, double *yexp, double *y, double *ystd, double *res, double *reserr);

static int check_flag(void *flagvalue, char *funcname, int opt);
static int ewt(N_Vector y, N_Vector w, void *user_data);

/* user functions */
#include "arSimuCalcFunctions.c"

void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[]) {
    
    int nthreads_x, nthreads_y, nm, im, nc, ic, tid, dtid, nd, id, rc, has_tExp;
    
    mxArray    *arconfig;
    mxArray    *arcondition;
    mxArray    *ardata;
    
    /* get ar.model */
    armodel = mxGetField(prhs[0], 0, "model");
    if(armodel==NULL){
        mexErrMsgTxt("field ar.model not existing");
    }
    
    fine = (int) mxGetScalar(prhs[1]);
    sensi = (int) mxGetScalar(prhs[2]);
    
    /* get ar.config */
    arconfig = mxGetField(prhs[0], 0, "config");
    parallel = (int) mxGetScalar(mxGetField(arconfig, 0, "useParallel"));
    jacobian = (int) mxGetScalar(mxGetField(arconfig, 0, "useJacobian"));
    cvodes_rtol = mxGetScalar(mxGetField(arconfig, 0, "rtol"));
    cvodes_atol = mxGetScalar(mxGetField(arconfig, 0, "atol"));
    cvodes_maxsteps = (int) mxGetScalar(mxGetField(arconfig, 0, "maxsteps"));
    fiterrors = (int) mxGetScalar(mxGetField(arconfig, 0, "fiterrors"));
    fiterrors_correction = (double) mxGetScalar(mxGetField(arconfig, 0, "fiterrors_correction"));
            
    /* threads */
    nthreads_x = mxGetScalar(mxGetField(arconfig, 0, "nthreads_x"));
    nthreads_y = mxGetScalar(mxGetField(arconfig, 0, "nthreads_y"));
    pthread_t threads_x[nthreads_x];
    pthread_t threads_y[nthreads_y];
    struct thread_data_x thread_data_x_array[nthreads_x];
    struct thread_data_y thread_data_y_array[nthreads_y];
    
/*    printf("%i x-threads (%i fine, %i sensi, %i jacobian, %g rtol, %g atol, %i maxsteps)\n", nthreads_x, fine,
            sensi, jacobian, cvodes_rtol, cvodes_atol, cvodes_maxsteps);
    printf("%i y-threads\n", nthreads_y); */
    
    nm = mxGetNumberOfElements(armodel);
    /* loop over models */
    for(im=0; im<nm; ++im){
        
        /* get ar.model(im).condition */
        arcondition = mxGetField(armodel, im, "condition");
        if(arcondition==NULL){
            mexErrMsgTxt("field ar.model.condition not existing");
        }
        
        nc = mxGetNumberOfElements(arcondition);
        /* loop over conditions */
        for(ic=0; ic<nc; ++ic){
            has_tExp = (int) mxGetScalar(mxGetField(arcondition, ic, "has_tExp"));
            
            if(has_tExp == 1 | fine == 1) {
                tid = mxGetScalar(mxGetField(arcondition, ic, "thread_id"));
                
                thread_data_x_array[tid].im = im;
                thread_data_x_array[tid].ic = ic;
                thread_data_x_array[tid].arcondition = arcondition;
                
                if(parallel==1){
                    /* printf("creating condition thread %i, m=%i, c=%i\n", tid, im, ic); */
                    rc = pthread_create(&threads_x[tid], NULL, x_calc, (void *) &thread_data_x_array[tid]);
                    if (rc){
                        mexErrMsgTxt("ERROR at pthread_create");
                    }
                } else {
                    x_calc(&thread_data_x_array[tid]);
                }
            }
        }
    }
    
    /* wait for termination of condition threads = pthread_exit(NULL);*/
    if(parallel==1){
        /* loop over models */
        for(im=0; im<nm; ++im){
            
            /* get ar.model(im).condition */
            arcondition = mxGetField(armodel, im, "condition");
            if(arcondition==NULL){
                mexErrMsgTxt("field ar.model.condition not existing");
            }
            
            nc = mxGetNumberOfElements(arcondition);
            /* loop over conditions */
            for(ic=0; ic<nc; ++ic){
                has_tExp = (int) mxGetScalar(mxGetField(arcondition, ic, "has_tExp"));
                
                if(has_tExp == 1 | fine == 1) {
                    tid = mxGetScalar(mxGetField(arcondition, ic, "thread_id"));
                    
                    rc = pthread_join(threads_x[tid], NULL);
                    if (rc){
                        mexErrMsgTxt("ERROR at pthread_join");
                    }
                }
            }
        }
    }
    
    /* loop over models */
    for(im=0; im<nm; ++im){
        /* get ar.model(im).data */
        ardata = mxGetField(armodel, im, "data");
        
        if(ardata!=NULL){
            arcondition = mxGetField(armodel, im, "condition");
            
            nd = mxGetNumberOfElements(ardata);
            /* loop over data */
            for(id=0; id<nd; ++id){
                has_tExp = (int) mxGetScalar(mxGetField(ardata, id, "has_tExp"));
                
                if(has_tExp == 1 | fine == 1) {
                    tid = mxGetScalar(mxGetField(ardata, id, "thread_id"));
                    
                    thread_data_y_array[tid].im = im;
                    thread_data_y_array[tid].id = id;
                    thread_data_y_array[tid].ardata = ardata;
                    thread_data_y_array[tid].arcondition = arcondition;
                    
                    dtid = mxGetScalar(mxGetField(ardata, id, "dthread_id"));
                    thread_data_y_array[tid].dt = dtid;
                    
                    if(parallel==1){
                        /* printf("creating data thread %i, m=%i, d=%i (wait call for condition #%i)\n", tid, im, id, dtid); */
                        rc = pthread_create(&threads_y[tid], NULL, y_calc, (void *) &thread_data_y_array[tid]);
                        if (rc){
                            mexErrMsgTxt("ERROR at pthread_create");
                        }
                    } else {
                        y_calc(&thread_data_y_array[tid]);
                    }
                }
            }
        }
    }
    
    /* wait for termination of data threads = pthread_exit(NULL);*/
    if(parallel==1){
        /* loop over models */
        for(im=0; im<nm; ++im){
            /* get ar.model(im).data */
            ardata = mxGetField(armodel, im, "data");
            
            if(ardata!=NULL){
                arcondition = mxGetField(armodel, im, "condition");
                
                nd = mxGetNumberOfElements(ardata);
                /* loop over data */
                for(id=0; id<nd; ++id){
                    has_tExp = (int) mxGetScalar(mxGetField(ardata, id, "has_tExp"));
                    
                    if(has_tExp == 1 | fine == 1) {
                        tid = mxGetScalar(mxGetField(ardata, id, "thread_id"));
                        
                        rc = pthread_join(threads_y[tid], NULL);
                        if (rc){
                            mexErrMsgTxt("ERROR at pthread_join");
                        }
                    }
                }
            }
            
        }
    }
}



/* calculate dynamics by calling CVODES */
void *x_calc(void *threadarg) {
    struct thread_data_x *my_data = (struct thread_data_x *) threadarg;
    
    int im = my_data->im;
    int ic = my_data->ic;
    mxArray *arcondition = my_data->arcondition;
    
    /* printf("computing model #%i, condition #%i\n", im, ic); */
    
    /* begin of CVODES */
    void *cvode_mem;
    UserData data;
    
    int flag;
    int is, js, ks;
    int nout, neq;
    
    realtype t;
    double tstart;
    N_Vector x;
    N_Vector *sx;
    realtype *sxtmp;
    
    double *status;
    double *ts;
    double *returnu;
    double *returnsu;
    double *returnv;
    double *returnsv;
    double *returnx;
    double *returnsx;
    double *returndxdt;
    double *returnddxdtdp;
    
    int sensi_meth = CV_SIMULTANEOUS; /* CV_SIMULTANEOUS or CV_STAGGERED */
    
    /* MATLAB values */
    status = mxGetData(mxGetField(arcondition, ic, "status"));
    tstart = mxGetScalar(mxGetField(arcondition, ic, "tstart"));
    neq = mxGetNumberOfElements(mxGetField(armodel, im, "xs"));
    
    if(fine == 1){
        ts = mxGetData(mxGetField(arcondition, ic, "tFine"));
        nout = mxGetNumberOfElements(mxGetField(arcondition, ic, "tFine"));
        
        returnu = mxGetData(mxGetField(arcondition, ic, "uFineSimu"));
        returnv = mxGetData(mxGetField(arcondition, ic, "vFineSimu"));
        returnx = mxGetData(mxGetField(arcondition, ic, "xFineSimu"));
        if (sensi == 1) {
            returnsu = mxGetData(mxGetField(arcondition, ic, "suFineSimu"));
            returnsv = mxGetData(mxGetField(arcondition, ic, "svFineSimu"));
            returnsx = mxGetData(mxGetField(arcondition, ic, "sxFineSimu"));
        }
    }
    else{
        ts = mxGetData(mxGetField(arcondition, ic, "tExp"));
        nout = mxGetNumberOfElements(mxGetField(arcondition, ic, "tExp"));
        
        returnu = mxGetData(mxGetField(arcondition, ic, "uExpSimu"));
        returnv = mxGetData(mxGetField(arcondition, ic, "vExpSimu"));
        returnx = mxGetData(mxGetField(arcondition, ic, "xExpSimu"));
        if (sensi == 1) {
            returnsu = mxGetData(mxGetField(arcondition, ic, "suExpSimu"));
            returnsv = mxGetData(mxGetField(arcondition, ic, "svExpSimu"));
            returnsx = mxGetData(mxGetField(arcondition, ic, "sxExpSimu"));
        }
    }
    returndxdt = mxGetData(mxGetField(arcondition, ic, "dxdt"));
    if (sensi == 1) {
        returnddxdtdp = mxGetData(mxGetField(arcondition, ic, "ddxdtdp"));
    }
    
    /* User data structure */
    status[0] = 1;
    data = (UserData) malloc(sizeof *data);
    if (check_flag((void *)data, "malloc", 2)) {if(parallel==1) {pthread_exit(NULL);} return;}
    
    data->u = mxGetData(mxGetField(arcondition, ic, "uNum"));
    int nu = mxGetNumberOfElements(mxGetField(arcondition, ic, "uNum"));
    
    data->p = mxGetData(mxGetField(arcondition, ic, "pNum"));
    int np = mxGetNumberOfElements(mxGetField(arcondition, ic, "pNum"));
    int nps = np;
    
    data->v = mxGetData(mxGetField(arcondition, ic, "vNum"));
    int nv = mxGetNumberOfElements(mxGetField(arcondition, ic, "vNum"));
    data->dvdx = mxGetData(mxGetField(arcondition, ic, "dvdxNum"));
    data->dvdu = mxGetData(mxGetField(arcondition, ic, "dvduNum"));
    data->dvdp = mxGetData(mxGetField(arcondition, ic, "dvdpNum"));
    
    /* fill for t=0 */
    fu(data, 0.0, im, ic);
    
    if(neq>0){
        /* Initial conditions */
        status[0] = 2;
        x = N_VNew_Serial(neq);
        if (check_flag((void *)x, "N_VNew_Serial", 0)) {if(parallel==1) {pthread_exit(NULL);} return;}
        for (is=0; is<neq; is++) Ith(x, is+1) = 0.0;
        fx0(x, data, im, ic);
        fv(data, 0.0, x, im, ic);
        fx(0.0, x, returndxdt, data, im, ic);
        
        /* Create CVODES object */
        status[0] = 3;
        cvode_mem = CVodeCreate(CV_BDF, CV_NEWTON);
        if (check_flag((void *)cvode_mem, "CVodeCreate", 0)) {if(parallel==1) {pthread_exit(NULL);} return;}
        
        /* Allocate space for CVODES */
        status[0] = 4;
        flag = AR_CVodeInit(cvode_mem, x, tstart, im, ic);
        if (check_flag(&flag, "CVodeInit", 1)) {if(parallel==1) {pthread_exit(NULL);} return;}
        
        /* Use private function to compute error weights */
        status[0] = 5;
        flag = CVodeSStolerances(cvode_mem, RCONST(cvodes_rtol), RCONST(cvodes_atol));
        if (check_flag(&flag, "CVodeSetTol", 1)) {if(parallel==1) {pthread_exit(NULL);} return;}
        
        /* Attach user data */
        status[0] = 6;
        flag = CVodeSetUserData(cvode_mem, data);
        if (check_flag(&flag, "CVodeSetUserData", 1)) {if(parallel==1) {pthread_exit(NULL);} return;}
        
        /* Attach linear solver */
        status[0] = 7;
        flag = CVDense(cvode_mem, neq);
        if (check_flag(&flag, "CVDense", 1)) {if(parallel==1) {pthread_exit(NULL);} return;}
        
        /* Jacobian-related settings */
        if (jacobian == 1) {
            status[0] = 8;
            flag = AR_CVDlsSetDenseJacFn(cvode_mem, im, ic);
            if (check_flag(&flag, "CVDlsSetDenseJacFn", 1)) {if(parallel==1) {pthread_exit(NULL);} return;}
        }
        
        /* custom error weight function */
        /*
        flag = CVodeWFtolerances(cvode_mem, ewt);
        if (check_flag(&flag, "CVodeWFtolerances", 1)) {if(parallel==1) {pthread_exit(NULL);} return;}
        */
    }
    
    /* Sensitivity-related settings */
    if (sensi == 1) {
        /* User data structure */
        data->su = mxGetData(mxGetField(arcondition, ic, "suNum"));
        data->sv = mxGetData(mxGetField(arcondition, ic, "svNum"));
        
        /* fill inputs */
        fsu(data, 0.0, im, ic);
        
        if(neq>0){
            /* Load sensitivity initial conditions */
            status[0] = 9;
            sx = N_VCloneVectorArray_Serial(nps, x);
            if (check_flag((void *)sx, "N_VCloneVectorArray_Serial", 0)) {if(parallel==1) {pthread_exit(NULL);} return;}
            for(js=0; js < nps; js++) {
                sxtmp = NV_DATA_S(sx[js]);
                for(ks=0; ks < neq; ks++) {
                    sxtmp[ks] = 0.0;
                }
            }
            for (is=0;is<nps;is++) fsx0(is, sx[is], data, im, ic);
            fsv(data, 0.0, x, im, ic);
            dfxdp(data, 0.0, x, returnddxdtdp, im, ic);
            
            status[0] = 10;
            flag = AR_CVodeSensInit1(cvode_mem, nps, sensi_meth, sx, im, ic);
            if(check_flag(&flag, "CVodeSensInit1", 1)) {if(parallel==1) {pthread_exit(NULL);} return;}
            
            status[0] = 11;
            flag = CVodeSensEEtolerances(cvode_mem);
            if(check_flag(&flag, "CVodeSensEEtolerances", 1)) {if(parallel==1) {pthread_exit(NULL);} return;}
            
            status[0] = 13;
            flag = CVodeSetSensParams(cvode_mem, data->p, NULL, NULL);
            if (check_flag(&flag, "CVodeSetSensParams", 1)) {if(parallel==1) {pthread_exit(NULL);} return;}
        }
    }
    
    if(neq>0){
        /* Number of maximal internal steps */
        status[0] = 15;
        flag = CVodeSetMaxNumSteps(cvode_mem, cvodes_maxsteps);
        if(check_flag(&flag, "CVodeSetMaxNumSteps", 1)) {if(parallel==1) {pthread_exit(NULL);} return;}
    }
    
    /* loop over output points */
    for (is=0; is < nout; is++) {
        /* printf("%f x-loop (im=%i ic=%i)\n", ts[is], im, ic); */
        
        /* only integrate after tstart */
        if(ts[is] > tstart) {
            if(neq>0) {
                flag = CVode(cvode_mem, RCONST(ts[is]), x, &t, CV_NORMAL);
                status[0] = flag;
                if(flag==-1) printf("CV_TOO_MUCH_WORK stoped at t=%f, did not reach output time after %i steps (m=%i, c=%i).\n", t, cvodes_maxsteps, im, ic);
                if(flag<-1) printf("CVODES stoped at t=%f (m=%i, c=%i).\n", t, im, ic);
                if (check_flag(&flag, "CVode", 1)) {
                    /* Free memory */
                    N_VDestroy_Serial(x);
                    if (sensi == 1) {
                        N_VDestroyVectorArray_Serial(sx, nps);
                    }
                    CVodeFree(&cvode_mem);
                    free(data);

                    if(parallel==1) {
                        pthread_exit(NULL);
                    } 
                    return;
                }
            }
        }
        fu(data, ts[is], im, ic);
        fv(data, ts[is], x, im, ic);

        
        /* set output values */
        for(js=0; js < nu; js++) returnu[js*nout+is] = data->u[js];
        if(nv>0) { for(js=0; js < nv; js++) returnv[js*nout+is] = data->v[js]; }
        if(neq>0) { for(js=0; js < neq; js++) returnx[js*nout+is] = Ith(x, js+1); }
        
        /* set output sensitivities */
        if (sensi == 1) {
            if(ts[is] > tstart) {
                if(neq>0) {
                    status[0] = 14;
                    flag = CVodeGetSens(cvode_mem, &t, sx);
                    if (check_flag(&flag, "CVodeGetSens", 1)) {if(parallel==1) {pthread_exit(NULL);} return;}
                }
            }
            fsu(data, ts[is], im, ic);
            fsv(data, ts[is], x, im, ic);
            
            for(js=0; js < nps; js++) {
                if(neq>0) {
                    sxtmp = NV_DATA_S(sx[js]);
                    for(ks=0; ks < neq; ks++) {
                        returnsx[js*neq*nout + ks*nout + is] = sxtmp[ks];
                    }
                }
                for(ks=0; ks < nu; ks++) {
                    returnsu[js*nu*nout + ks*nout + is] = data->su[(js*nu)+ks];
                }
            }
        }
    }
    
    status[0] = 0;
    
    /* Free memory */
    if(neq>0) {
        N_VDestroy_Serial(x);
        if (sensi == 1) {
            N_VDestroyVectorArray_Serial(sx, nps);
        }
        CVodeFree(&cvode_mem);
    }
    free(data);
    
    /* end of CVODES */
    
    /* printf("computing model #%i, condition #%i (done)\n", im, ic); */
    
    if(parallel==1) {pthread_exit(NULL);}
}



/* calculate observations */
void *y_calc(void *threadarg) {
    struct thread_data_y *my_data = (struct thread_data_y *) threadarg;
    
    int im = my_data->im;
    int id = my_data->id;
    int dt = my_data->dt;
    int rc;   
    mxArray *ardata = my_data->ardata;
    mxArray *arcondition = my_data->arcondition;
    
    
    /* wait for dependend x thread */
/*    printf("computing model #%i, data #%i (waiting for %i)\n", im, id, dt);
    if(parallel==1) {
        rc = pthread_join(threads_x[dt], NULL);
        if (rc){
            mexErrMsgTxt("ERROR at pthread_join");
        }
    }*/
    
    /* printf("computing model #%i, data #%i\n", im, id); */
    
    int nt, it, iy, ip, ntlink, itlink;
    int ny, np, ic;
    int has_yExp;
    
    double *t;
    double *tlink;
    
    double *y;
    double *ystd;
    double *yexp;
    double *res;
    double *reserr;
    
    double *sy;
    double *systd;
    double *sres;
    double *sreserr;
    
    double *qlogy;
    double *qlogp;
    
    double *p;
    double *u;
    double *x;
    double *su;
    double *sx;
    
    double *chi2;
    double *chi2err;
    
    /* MATLAB values */
    ic = (int) mxGetScalar(mxGetField(ardata, id, "cLink")) - 1;
    has_yExp = (int) mxGetScalar(mxGetField(ardata, id, "has_yExp"));
    
    ny = mxGetNumberOfElements(mxGetField(ardata, id, "y"));
    qlogy = mxGetData(mxGetField(ardata, id, "logfitting"));
    qlogp = mxGetData(mxGetField(ardata, id, "qLog10"));
    p = mxGetData(mxGetField(ardata, id, "pNum"));
    np = mxGetNumberOfElements(mxGetField(ardata, id, "pNum"));
    
    if(fine == 1){
        t = mxGetData(mxGetField(ardata, id, "tFine"));
        nt = mxGetNumberOfElements(mxGetField(ardata, id, "tFine"));
        tlink = mxGetData(mxGetField(ardata, id, "tLinkFine"));
        ntlink = mxGetNumberOfElements(mxGetField(arcondition, ic, "tFine"));
        
        y = mxGetData(mxGetField(ardata, id, "yFineSimu"));
        ystd = mxGetData(mxGetField(ardata, id, "ystdFineSimu"));
        
        u = mxGetData(mxGetField(arcondition, ic, "uFineSimu"));
        x = mxGetData(mxGetField(arcondition, ic, "xFineSimu"));
        
        if (sensi == 1) {
            sy = mxGetData(mxGetField(ardata, id, "syFineSimu"));
            systd = mxGetData(mxGetField(ardata, id, "systdFineSimu"));
            
            su = mxGetData(mxGetField(arcondition, ic, "suFineSimu"));
            sx = mxGetData(mxGetField(arcondition, ic, "sxFineSimu"));
        }
    }
    else{
        t = mxGetData(mxGetField(ardata, id, "tExp"));
        nt = mxGetNumberOfElements(mxGetField(ardata, id, "tExp"));
        tlink = mxGetData(mxGetField(ardata, id, "tLinkExp"));
        ntlink = mxGetNumberOfElements(mxGetField(arcondition, ic, "tExp"));
        
        y = mxGetData(mxGetField(ardata, id, "yExpSimu"));
        ystd = mxGetData(mxGetField(ardata, id, "ystdExpSimu"));
        
        u = mxGetData(mxGetField(arcondition, ic, "uExpSimu"));
        x = mxGetData(mxGetField(arcondition, ic, "xExpSimu"));
        
        if (sensi == 1) {
            sy = mxGetData(mxGetField(ardata, id, "syExpSimu"));
            systd = mxGetData(mxGetField(ardata, id, "systdExpSimu"));
            
            su = mxGetData(mxGetField(arcondition, ic, "suExpSimu"));
            sx = mxGetData(mxGetField(arcondition, ic, "sxExpSimu"));
        }
        
        if (has_yExp == 1) {
            yexp = mxGetData(mxGetField(ardata, id, "yExp"));
            if(fiterrors==-1) ystd = mxGetData(mxGetField(ardata, id, "yExpStd"));
            
            res = mxGetData(mxGetField(ardata, id, "res"));
            reserr = mxGetData(mxGetField(ardata, id, "reserr"));
            if (sensi == 1) {
                sres = mxGetData(mxGetField(ardata, id, "sres"));
                sreserr = mxGetData(mxGetField(ardata, id, "sreserr"));
            }
            chi2 = mxGetData(mxGetField(ardata, id, "chi2"));
            chi2err = mxGetData(mxGetField(ardata, id, "chi2err"));
            for(iy=0; iy<ny; iy++) {
                chi2[iy] = 0.0;
                if(fiterrors==1) chi2err[iy] = 0.0;
            }
        }
    }
    
    /* loop over output points */
    for (it=0; it < nt; it++) {
        /* printf("%f y-loop (im=%i id=%i)\n", t[it], im, id); */
        itlink = (int) tlink[it] - 1;
        
        fy(t[it], nt, it, ntlink, itlink, 0, 0, 0, y, p, u, x, im, id);
        
        /* log trafo of y */
        for (iy=0; iy<ny; iy++) {
            if(qlogy[iy] > 0.5){
                y[it + (iy*nt)] = log10(y[it + (iy*nt)]);
            }
        }
        
        if(fiterrors!=-1) fystd(t[it], nt, it, ntlink, itlink, ystd, y, p, u, x, im, id);
        
        if (sensi == 1) {
            fsy(t[it], nt, it, ntlink, itlink, sy, p, u, x, su, sx, im, id);
            
            /* log trafo of sy */
            for (iy=0; iy<ny; iy++) {
                if(qlogy[iy] > 0.5) {
                    for (ip=0; ip < np; ip++) {
                        sy[it + (iy*nt) + (ip*nt*ny)] =
                                sy[it + (iy*nt) + (ip*nt*ny)]
                                / pow(10.0, y[it + (iy*nt)])
                                / log(10.0);
                    }
                }
            }
            
            if(fiterrors!=-1) fsystd(t[it], nt, it, ntlink, itlink, systd, p, y, u, x, sy, su, sx, im, id);
        }
        
        if (has_yExp == 1 & fine == 0) {
            fres(nt, ny, it, res, y, yexp, ystd, chi2);
            if(sensi == 1) fsres(nt, ny, np, it, sres, sy, yexp, ystd);
            
            if(fiterrors==1) {
                fres_error(nt, ny, it, reserr, res, y, yexp, ystd, chi2err);
                if (sensi == 1) fsres_error(nt, ny, np, it, sres, sreserr, sy, systd, y, yexp, ystd, res, reserr);
            }
        }
        
        /* log trafo of parameters */
        if (sensi == 1 & has_yExp == 1 & fine == 0) {
            for (ip=0; ip < np; ip++) {
                if (qlogp[ip] > 0.5) {
                    for (iy=0; iy<ny; iy++) {
                        sres[it + (iy*nt) + (ip*nt*ny)] *= p[ip] * log(10.0);
                        if(fiterrors==1) sreserr[it + (iy*nt) + (ip*nt*ny)] *= p[ip] * log(10.0);
                    }
                }
            }
        }
    }
    /* printf("computing model #%i, data #%i (done)\n", im, id); */
    
    if(parallel==1) pthread_exit(NULL);
}

/* standard least squares */
static void fres(int nt, int ny, int it, double *res, double *y, double *yexp, double *ystd, double *chi2) {
    int iy;
    
    for(iy=0; iy<ny; iy++){
        res[it + (iy*nt)] = (yexp[it + (iy*nt)] - y[it + (iy*nt)]) / ystd[it + (iy*nt)] * sqrt(fiterrors_correction);
        if(mxIsNaN(yexp[it + (iy*nt)])) {
            res[it + (iy*nt)] = 0.0;
            y[it + (iy*nt)] = 0.0/0.0;
            ystd[it + (iy*nt)] = 0.0/0.0;
        }
        chi2[iy] += pow(res[it + (iy*nt)], 2);
    }
}
static void fsres(int nt, int ny, int np, int it, double *sres, double *sy, double *yexp, double *ystd) {
    int iy, ip;
    
    for(iy=0; iy<ny; iy++){
        for(ip=0; ip<np; ip++){
            sres[it + (iy*nt) + (ip*nt*ny)] = - sy[it + (iy*nt) + (ip*nt*ny)] / ystd[it + (iy*nt)] * sqrt(fiterrors_correction);
            if(mxIsNaN(yexp[it + (iy*nt)])) {
                sres[it + (iy*nt) + (ip*nt*ny)] = 0.0;
            }
        }
    }
}

/* least squares for error model fitting */
static void fres_error(int nt, int ny, int it, double *reserr, double *res, double *y, double *yexp, double *ystd, double *chi2err) {
    int iy;
    
    double add_c = 50.0;
    
    for(iy=0; iy<ny; iy++){
        reserr[it + (iy*nt)] = 2.0*log(ystd[it + (iy*nt)]);
        if(mxIsNaN(yexp[it + (iy*nt)])) {
            reserr[it + (iy*nt)] = 0.0;
            y[it + (iy*nt)] = 0.0/0.0;
            ystd[it + (iy*nt)] = 0.0/0.0;
        } else {
            reserr[it + (iy*nt)] += add_c;
            if(reserr[it + (iy*nt)] < 0) mexErrMsgTxt("ERROR error model < 1e-10 not allowed"); /* s*log(ystd) + add_c > 0 */
            reserr[it + (iy*nt)] = sqrt(reserr[it + (iy*nt)]);
            chi2err[iy] += pow(reserr[it + (iy*nt)], 2) - add_c;
        }
    }
}
static void fsres_error(int nt, int ny, int np, int it, double *sres, double *sreserr, double *sy, double *systd, double *yexp, double *y, double *ystd, double *res, double *reserr) {
    int iy, ip;
    double rtmp;
    
    for(iy=0; iy<ny; iy++){
        for(ip=0; ip<np; ip++){
            sres[it + (iy*nt) + (ip*nt*ny)] -= systd[it + (iy*nt) + (ip*nt*ny)] * res[it + (iy*nt)] / ystd[it + (iy*nt)];
            sreserr[it + (iy*nt) + (ip*nt*ny)] = systd[it + (iy*nt) + (ip*nt*ny)] / (reserr[it + (iy*nt)] * ystd[it + (iy*nt)]);
            
            if(mxIsNaN(yexp[it + (iy*nt)])) {
                sres[it + (iy*nt) + (ip*nt*ny)] = 0.0;
                sreserr[it + (iy*nt) + (ip*nt*ny)] = 0.0;
            }
        }
    }
}


/*
 * Check function return value of CVODES.
 *    opt == 0 means SUNDIALS function allocates memory so check if
 *             returned NULL pointer
 *    opt == 1 means SUNDIALS function returns a flag so check if
 *             flag >= 0
 *    opt == 2 means function allocates memory so check if returned
 *             NULL pointer
 */

static int check_flag(void *flagvalue, char *funcname, int opt) {
    int *errflag;
    
    /* Check if SUNDIALS function returned NULL pointer - no memory allocated */
    if (opt == 0 && flagvalue == NULL) {
        /* printf("\nSUNDIALS ERROR: %s() failed - returned NULL pointer\n\n", funcname); */
        return(1);
    }
    
    /* Check if flag < 0 */
    else if (opt == 1) {
        errflag = (int *) flagvalue;
        if (*errflag < 0) {
            /* printf("\nSUNDIALS ERROR: %s() failed with flag = %d\n\n", funcname, *errflag); */
            return(1);
        }
    }
    
    /* Check if function returned NULL pointer - no memory allocated */
    else if (opt == 2 && flagvalue == NULL) {
        /* printf("\nMEMORY ERROR: %s() failed - returned NULL pointer\n\n", funcname); */
        return(1);
    }
    
    return(0);
}

/* custom error weight function
static int ewt(N_Vector y, N_Vector w, void *user_data)
{
  int i;
  realtype yy, ww;
  
  for (i=1; i<=NV_LENGTH_S(y); i++) {
    yy = Ith(y,i);
    ww = cvodes_rtol * ABS(yy) + cvodes_atol;  
    if (ww <= 0.0) return (-1);
    Ith(w,i) = 1.0/ww;
  }

  return(0);
} 
*/