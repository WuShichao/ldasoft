
/***************************  REQUIRED LIBRARIES  ***************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include <gsl/gsl_rng.h>
#include <gsl/gsl_randist.h>

#include "omp.h"

/*************  PROTOTYPE DECLARATIONS FOR INTERNAL FUNCTIONS  **************/

#include "LISA.h"
#include "GalacticBinary.h"
#include "GalacticBinaryIO.h"
#include "GalacticBinaryData.h"
#include "GalacticBinaryPrior.h"
#include "GalacticBinaryModel.h"
#include "GalacticBinaryProposal.h"
#include "GalacticBinaryWaveform.h"

#define FIXME 0

void ptmcmc(struct Model ****model, struct Chain *chain, struct Flags *flags);
void adapt_temperature_ladder(struct Chain *chain, int mcmc);

void galactic_binary_mcmc(struct Orbit *orbit, struct Data *data, struct Model **model, struct Model **trial, struct Chain *chain, struct Flags *flags, int ic);
void data_mcmc(struct Orbit *orbit, struct Data **data, struct Model ***model, struct Chain *chain, struct Flags *flags, int ic);
void noise_model_mcmc(struct Orbit *orbit, struct Data *data, struct Model *model, struct Model *trial, struct Chain *chain, int ic, int Nseg);


/* ============================  MAIN PROGRAM  ============================ */

int main(int argc, char *argv[])
{
  
  time_t start, stop;
  start = time(NULL);
  
  int NC    = 8;    //number of chains
  int NMAX  = 10;   //max number of waveforms
  int NMCMC = 5000; //number of MCMC steps
  int NBURN = 5000; //number of burn-in steps

  
  /* Allocate data structures */
  struct Flags *flags   = malloc(sizeof(struct Flags));
  struct Orbit *orbit   = malloc(sizeof(struct Orbit));
  struct Chain *chain   = malloc(sizeof(struct Chain));
  struct Data  **data   = malloc(sizeof(struct Data*)*NMAX);
  struct Model ***trial = malloc(sizeof(struct Model**)*NC);
  struct Model ****model= malloc(sizeof(struct Model***)*NC);
  
  
  /* Parse command line and set defaults/flags */
  for(int i=0; i<NMAX; i++) data[i] = malloc(sizeof(struct Data));
  parse(argc,argv,data,orbit,flags,NMAX);
  
  
  /* Load spacecraft ephemerides */
  initialize_orbit(orbit);
  
  
  /* Initialize data structures */
  alloc_data(data, NMCMC, NMAX);

  
  /* Inject gravitational wave signal */
  GalacticBinaryInjectVerificationSource(data,orbit,flags);

  
  /* Initialize parallel chain */
  initialize_chain(chain, flags, &data[0]->cseed, NC);
  
  
  /* Initialize data models */
  int ic;
#pragma omp parallel for private(ic) shared(model,chain,data,orbit,trial)
  for(ic=0; ic<NC; ic++)
  {
    trial[ic] = malloc(sizeof(struct Model * ) * data[0]->Nsegment);
    for(int nseg = 0; nseg < data[0]->Nsegment; nseg++)
    {
      trial[ic][nseg] = malloc(sizeof(struct Model));
      alloc_model(trial[ic][nseg],NMAX,data[0]->N,data[0]->Nchannel);
    }

    
    model[ic] = malloc(sizeof(struct Model **) * flags->injection);
    
    for(int i=0; i<flags->injection; i++)
    {
      model[ic][i] = malloc(sizeof(struct Model *) * flags->segment);
      for(int nseg = 0; nseg < data[0]->Nsegment; nseg++)
      {
        model[ic][i][nseg] = malloc(sizeof(struct Model));
        
        struct Model *model_ptr = model[ic][i][nseg];
        struct Data  *data_ptr  = data[i];
        
        alloc_model(model_ptr,NMAX,data_ptr->N,data_ptr->Nchannel);
        
        set_uniform_prior(model_ptr, data_ptr);
        
        //set noise model
        copy_noise(data[0]->noise[nseg], model_ptr->noise);
        
        //set t0
        model_ptr->t0 = 0;//-0.5;//model[ic]->t0_min + gsl_rng_uniform(chain->r[ic])*(model[ic]->t0_max - model[ic]->t0_min);
        
        
        //set signal model
        for(int n=0; n<NMAX; n++)
        {
          draw_from_prior(model_ptr, model_ptr->source[n], model_ptr->source[n]->params, chain->r[ic]);
          galactic_binary_fisher(orbit, data_ptr, model_ptr->source[n], data_ptr->noise[nseg]);
          map_array_to_params(model_ptr->source[n], model_ptr->source[n]->params, data_ptr->T);
        }
        
        // Form master model & compute likelihood of starting position
        generate_noise_model(data_ptr, model_ptr);
        generate_signal_model(orbit, data_ptr, model_ptr);
        
        model_ptr->logL     = gaussian_log_likelihood(orbit, data_ptr, model_ptr, 0);
        model_ptr->logLnorm = gaussian_log_likelihood_constant_norm(data_ptr, model_ptr);
        
        if(ic==0) chain->logLmax += model_ptr->logL + model_ptr->logLnorm;
        
      }//end loop over sources
    }//end loop over segments
  }//end loop over chains

  
  /* The MCMC loop */
  for(int mcmc = -NBURN; mcmc < NMCMC; mcmc++)
  {
#pragma omp parallel for private(ic) shared(model,chain,data,orbit,trial,flags)
    for(ic=0; ic<NC; ic++)
    {
      for(int i=0; i<flags->injection; i++)
      {
        struct Model **model_ptr = model[chain->index[ic]][i];
        struct Model **trial_ptr = trial[chain->index[ic]];
        
        for(int steps=0; steps < 100; steps++)
        {
          galactic_binary_mcmc(orbit, data[i], model_ptr, trial_ptr, chain, flags, ic);

          /*for(int j=0; j<flags->segment; j++)
            noise_model_mcmc(orbit, data[i], model_ptr[j], trial_ptr[j], chain, ic, j);*/
        }//loop over MCMC steps
        
        //update fisher matrix for each chain
        for(int n=0; n<model_ptr[0]->Nlive; n++)
        {
          galactic_binary_fisher(orbit, data[i], model_ptr[0]->source[n], data[i]->noise[0]);
        }//loop over sources in model
      }//loop over frequency segments
      
      //update start time for data segments
      data_mcmc(orbit, data, model[chain->index[ic]], chain, flags, ic);

    }// (parallel) loop over chains
    
    ptmcmc(model,chain,flags);
    adapt_temperature_ladder(chain, mcmc+NBURN);

    print_chain_files(data[0], model, chain, flags, mcmc);
    
    //track maximum log Likelihood
    update_max_log_likelihood(model, chain, flags);
    
    //store reconstructed waveform
    print_waveform_draw(data, model[chain->index[0]], flags);

    if(mcmc>0 && mcmc%data[0]->downsample==0)
    {
      print_chain_state(data[0], chain, model[chain->index[0]][0], stdout, mcmc);
      for(int i=0; i<flags->injection; i++)save_waveforms(data[i], model[chain->index[0]][i][FIXME], mcmc/data[i]->downsample);
      for(ic=0; ic<NC; ic++)
      {
        for(int i=0; i<flags->injection; i++)
        {
          for(int j=0; j<data[0]->Nsegment; j++)
          {
            chain->avgLogL[ic] += model[chain->index[ic]][i][j]->logL + model[chain->index[ic]][i][j]->logLnorm;
          }
        }
      }
    }
  }
  
  //print aggregate run files/results
  for(int i=0; i<flags->injection; i++)print_waveforms_reconstruction(data[i],i);
  
  FILE *chainFile = fopen("avg_log_likelihood.dat","w");
  for(ic=0; ic<NC; ic++) fprintf(chainFile,"%lg %lg\n",1./chain->temperature[ic],chain->avgLogL[ic]/(double)(NMCMC/data[0]->downsample));
  fclose(chainFile);
  
  //print total run time
  stop = time(NULL);
  
  if(flags->verbose) printf(" ELAPSED TIME = %g second\n",(double)(stop-start));
  
  
  //free memory and exit cleanly
  for(ic=0; ic<NC; ic++)
  {
    free_model(model[ic][FIXME][FIXME]);
    free_model(trial[ic][FIXME]);
  }
  free_orbit(orbit);
  free_noise(data[0]->noise[FIXME]);
  free_tdi(data[0]->tdi[FIXME]);
  free_chain(chain,flags);
  //free(model[FIXME][FIXME]);
  //free(trial[FIXME][FIXME]);
  free(data[0]);
  
  return 0;
}

void ptmcmc(struct Model ****model, struct Chain *chain, struct Flags *flags)
{
  int a, b;
  int olda, oldb;
  
  double heat1, heat2;
  double logL1, logL2;
  double dlogL;
  double H;
  double alpha;
  double beta;
  
  int NC = chain->NC;
  
  //b = (int)(ran2(seed)*((double)(chain->NC-1)));
  for(b=1; b<NC; b++)
  {
    a = b - 1;
    chain->acceptance[a]=0;
    
    olda = chain->index[a];
    oldb = chain->index[b];
    
    heat1 = chain->temperature[a];
    heat2 = chain->temperature[b];
    
    logL1 = 0.0;
    logL2 = 0.0;
    for(int i=0; i<flags->injection; i++)
    {
      for(int j=0; j<flags->segment; j++)
      {
        logL1 += model[olda][i][j]->logL + model[olda][i][j]->logLnorm;
        logL2 += model[oldb][i][j]->logL + model[oldb][i][j]->logLnorm;
      }
    }
    
    //Hot chains jump more rarely
    if(gsl_rng_uniform(chain->r[a])<1.0)
    {
      dlogL = logL2 - logL1;
      H  = (heat2 - heat1)/(heat2*heat1);
      
      alpha = exp(dlogL*H);
      beta  = gsl_rng_uniform(chain->r[a]);
      
      //chain->ptprop[a]++;
      
      if(alpha >= beta)
      {
        //chain->ptacc[a]++;
        chain->index[a] = oldb;
        chain->index[b] = olda;
        chain->acceptance[a]=1;
      }
    }
  }
}

void adapt_temperature_ladder(struct Chain *chain, int mcmc)
{
  int ic;
  
  int NC = chain->NC;
  
  double S[NC];
  double A[NC][2];
  
  double nu=1;
  //double t0=100;
  double t0=1000.;
  
  for(ic=1; ic<NC-1; ic++)
  {
    S[ic] = log(chain->temperature[ic] - chain->temperature[ic-1]);
    A[ic][0] = chain->acceptance[ic-1];
    A[ic][1] = chain->acceptance[ic];
  }
  
  ic=0;
  for(ic=1; ic<NC-1; ic++)
  {
    S[ic] += (A[ic][0] - A[ic][1])*(t0/((double)mcmc+t0))/nu;
    //S[ic] += (A[ic][0] - A[ic][1])/nu;
    
    chain->temperature[ic] = chain->temperature[ic-1] + exp(S[ic]);
    
    if(chain->temperature[ic]/chain->temperature[ic-1] < 1.1) chain->temperature[ic] = chain->temperature[ic-1]*1.1;
  }//end loop over ic
}//end adapt function

void noise_model_mcmc(struct Orbit *orbit, struct Data *data, struct Model *model, struct Model *trial, struct Chain *chain, int ic, int Nseg)
{
  double logH  = 0.0; //(log) Hastings ratio
  double loga  = 1.0; //(log) transition probability
  
  double logPx  = 0.0; //(log) prior density for model x (current state)
  double logPy  = 0.0; //(log) prior density for model y (proposed state)
  
  //shorthand pointers
  struct Model *model_x = model;
  struct Model *model_y = trial;
  
  copy_model(model_x,model_y);
  
  //choose proposal distribution
  switch(data->Nchannel)
  {
    case 1:
      model_y->noise->etaX = model_x->noise->etaX + 0.1*gsl_ran_gaussian(chain->r[ic],1);
      break;
    case 2:
      model_y->noise->etaA = model_x->noise->etaA + 0.1*gsl_ran_gaussian(chain->r[ic],1);
      model_y->noise->etaE = model_x->noise->etaE + 0.1*gsl_ran_gaussian(chain->r[ic],1);
      break;
  }
  
  //get priors for x and y
  switch(data->Nchannel)
  {
    case 1:
      if(model_y->noise->etaX < 0.1 || model_y->noise->etaX>10) logPy=-INFINITY;
      break;
    case 2:
      if(model_y->noise->etaA < 0.1 || model_y->noise->etaA>10.) logPy=-INFINITY;
      if(model_y->noise->etaE < 0.1 || model_y->noise->etaE>10.) logPy=-INFINITY;
      break;
  }
  
  if(logPy > -INFINITY)
  {
    //  Form master template
    generate_noise_model(data, model_y);
    
    //get likelihood for y
    model_y->logL     = gaussian_log_likelihood(orbit, data, model_y, Nseg);
    model_y->logLnorm = gaussian_log_likelihood_constant_norm(data, model_y);
    
    /*
     H = [p(d|y)/p(d|x)]/T x p(y)/p(x) x q(x|y)/q(y|x)
     */
    logH += ( (model_y->logL+model_y->logLnorm) - (model_x->logL+model_x->logLnorm) )/chain->temperature[ic]; //delta logL
    logH += logPy  - logPx;                                         //priors
    
    loga = log(gsl_rng_uniform(chain->r[ic]));
    if(logH > loga) copy_model(model_y,model_x);
  }

}

void galactic_binary_mcmc(struct Orbit *orbit, struct Data *data, struct Model **model, struct Model **trial, struct Chain *chain, struct Flags *flags, int ic)
{
  double logH  = 0.0; //(log) Hastings ratio
  double loga  = 1.0; //(log) transition probability
  
  double logPx  = 0.0; //(log) prior density for model x (current state)
  double logPy  = 0.0; //(log) prior density for model y (proposed state)
  double logQyx = 0.0; //(log) proposal denstiy from x->y
  double logQxy = 0.0; //(log) proposal density from y->x

  //shorthand pointers
  struct Model **model_x = model;
  struct Model **model_y = trial;
  
  for(int i=0; i<data->Nsegment; i++)
  {
    copy_model(model_x[i],model_y[i]);
  }
  //pick a source to update
  int n = (int)(gsl_rng_uniform(chain->r[ic])*(double)model_x[0]->Nlive);
  
  //more shorthand pointers
  struct Source *source_x = model_x[0]->source[n];
  struct Source *source_y = model_y[0]->source[n];
  
  
  //choose proposal distribution
  double draw = gsl_rng_uniform(chain->r[ic]);
  if(draw < 0.1)
  /* no proposal density terms because proposal is symmetric */
    draw_from_prior(model_x[0], source_y, source_y->params, chain->r[ic]);
  else if(draw<0.9)
  /* no proposal density terms because proposal is symmetric */
    draw_from_fisher(model_x[0], source_x, source_y->params, chain->r[ic]);
  else
  /* no proposal density terms because proposal is symmetric */
    fm_shift(data, model_x[0], source_x, source_y->params, chain->r[ic]);

  //t0_shift(model_y[0], chain->r[ic]);
  
  map_array_to_params(source_y, source_y->params, data->T);

  //hold sky position fixed to injected value
  if(flags->fixSky)
  {
    source_y->costheta = data->inj[0]->costheta;
    source_y->phi      = data->inj[0]->phi;
    map_params_to_array(source_y, source_y->params, data->T);
  }
  
  //copy params for segment 0 into higher segments
  for(int i=1; i<data->Nsegment; i++)
  {
    copy_model(model_y[0],model_y[i]);
    model_y[i]->t0 = model_y[i-1]->t0 + data->T + data->tgap;
    map_array_to_params(model_y[i]->source[n], model_y[i]->source[n]->params, data->T);
  }

  //get priors for x and y
  logPx = evaluate_uniform_prior(model_x[0], source_x->params);
  logPy = evaluate_uniform_prior(model_y[0], source_y->params);

  if(logPy > -INFINITY)
  {
    for(int i=0; i<data->Nsegment; i++)
    {
      //  Form master template
      generate_signal_model(orbit, data, model_y[i]);
      
      //get likelihood for y
      model_y[i]->logL = gaussian_log_likelihood(orbit, data, model_y[i], i);
      
      /*
       H = [p(d|y)/p(d|x)]/T x p(y)/p(x) x q(x|y)/q(y|x)
       */
      logH += (model_y[i]->logL - model_x[i]->logL)/chain->temperature[ic]; //delta logL
    }
    logH += logPy  - logPx;  //priors
    logH += logQxy - logQyx; //proposals
    
    loga = log(gsl_rng_uniform(chain->r[ic]));
    if(logH > loga) for(int i=0; i<data->Nsegment; i++) copy_model(model_y[i],model_x[i]);
  }
}

void data_mcmc(struct Orbit *orbit, struct Data **data, struct Model ***model, struct Chain *chain, struct Flags *flags, int ic)
{
  double logH  = 0.0; //(log) Hastings ratio
  double loga  = 1.0; //(log) transition probability
  
  double tgap[flags->segment];
  
  struct Model ***trial = malloc(sizeof(struct Model **) * flags->injection);
  
  for(int i=0; i<flags->injection; i++)
  {
    trial[i] = malloc(sizeof(struct Model *) * flags->segment);
    for(int j = 0; j < flags->segment; j++)
    {
      trial[i][j] = malloc(sizeof(struct Model));
      
      alloc_model(trial[i][j],model[i][j]->Nmax,data[i]->N,data[i]->Nchannel);
      
      set_uniform_prior(trial[i][j], data[i]);
      
      copy_model(model[i][j],trial[i][j]);
    }
  }

  tgap[0]=data[0]->tgap;
  for(int i=1; i<flags->segment; i++)
  {
    for(int j=0; j<flags->injection; j++)
    {
      tgap[i] = data[0]->tgap;
      trial[j][i]->t0 = trial[j][i-1]->t0 + i*(data[0]->T) + tgap[i];
    }
  }
  
  for(int j=0; j<flags->injection; j++)
  {
    for(int i=0; i<flags->segment; i++)
    {
      //  Form master template
      generate_signal_model(orbit, data[j], model[j][i]);
      
      //get likelihood for y
      trial[j][i]->logL = gaussian_log_likelihood(orbit, data[j], trial[j][i], i);
      
      /*
       H = [p(d|y)/p(d|x)]/T x p(y)/p(x) x q(x|y)/q(y|x)
       */
      logH += (trial[j][i]->logL - model[j][i]->logL)/chain->temperature[ic]; //delta logL
    }
  }
  
  loga = log(gsl_rng_uniform(chain->r[ic]));
  if(logH > loga) for(int j=0; j<flags->injection; j++) for(int i=0; i<flags->segment; i++) copy_model(trial[j][i],model[j][i]);

  
  for(int i=0; i<flags->injection; i++)
  {
    for(int j = 0; j < flags->segment; j++)
    {
      free_model(trial[i][j]);
    }
    free(trial[i]);
  }
  free(trial);

}


