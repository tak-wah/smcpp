import numpy as np
from logging import getLogger
import jsonpickle
import functools
import multiprocessing
logger = getLogger(__name__)

from . import estimation_tools, _smcpp, util
from .estimation_result import EstimationResult
from .model import SMCModel

class Population(object):
    '''Class representing a population + model for estimation.'''
    def __init__(self, dataset_files, time_points, exponential_pieces, N0, mu, r, M, bounds, cmd_args):
        self._time_points = time_points
        self._exponential_pieces = exponential_pieces
        self._N0 = N0
        self._mu = mu
        self._r = r
        self._M = M
        self._bounds = bounds

        ## Parse each data set into an array of observations
        logger.info("Loading data...")
        dataset = util.parse_text_datasets(dataset_files)
        self._n = 2 + max([obs[:, -1].max() for obs in dataset])
        ## At this point, data have not been thinned or anything. 

        ## Initialize model
        self._model = SMCModel(time_points, exponential_pieces)

        ## After (potentially) doing pretraining, normalize and thin the data set
        ## Optionally thin each dataset
        if cmd_args.thinning is not None:
            logger.info("Thinning...")
            dataset = estimation_tools.thin_dataset(dataset, cmd_args.thinning)
        
        # Prepare empirical SFS for later use. This is cheap to compute
        esfs = util.compute_esfs(dataset, self._n)
        self._sfs = np.mean(esfs, axis=0)

        # pretrain if requested
        self._penalizer = functools.partial(estimation_tools.regularizer, 
                penalty=cmd_args.regularization_penalty,
                f=cmd_args.regularizer)

        theta_hat = 2. * N0 * 1e-8
        if not cmd_args.no_pretrain:
            logger.info("Pretraining")
            theta_hat = self._pretrain()
            logger.debug("inferred theta_hat: %g" % theta_hat)
    
        # We remember the initialized model for use in split estimated
        self._init_model_x = self.model.x.copy()

        ## choose hidden states based on prior model
        logger.info("Balancing hidden states...")
        self._balance_hidden_states()

        ## break up long spans
        self._dataset, attrs = estimation_tools.break_long_spans(dataset, 
                cmd_args.span_cutoff, cmd_args.length_cutoff)

        logger.debug("Average heterozygosity (derived / total bases) by data set:")
        for fn, key in zip(dataset_files, attrs):
            logger.debug(fn + ":")
            for attr in attrs[key]:
                logger.debug("%15d%15d%15d%12g%12g" % attr)

        ## Create inference object which will be used for all further calculations.
        logger.debug("Creating inference manager...")
        self._im = _smcpp.PyInferenceManager(self._n - 2, self._dataset, self._hidden_states)

        # Finally set parameters so they get propagated to the _im
        # Set theta once and for all
        if mu is None:
            self.theta = theta_hat
        else:
            self.theta = 2. * N0 * mu
        logger.debug("theta: %g" % self.theta)

        if r is None:
            r = self.theta / 4.
        # Initialize rho
        self.rho = 2 * N0 * r
        logger.debug("rho: %g" % self.rho)

        # Propagate changes to the inference manager
        self._im.model = self.model

    def _balance_hidden_states(self):
        hs = _smcpp.balance_hidden_states(self._model, self._M)
        cs = np.cumsum(self._model.s)
        cs = cs[cs <= hs[1]]
        self._hidden_states = np.sort(np.unique(np.concatenate([cs, hs])))
        logger.info("hidden states:\n%s" % str(self._hidden_states))

    def reset(self):
        self._model.x[:] = self._init_model_x[:]

    def penalize(self, model):
        return self._penalizer(model)

    def _pretrain(self):
        theta_hat = estimation_tools.pretrain(self._model, self._sfs, 
                self._bounds, 2. * self._N0 * 1e-8, self.penalize)
        return theta_hat

    def sfs(self):
        return self._sfs

    def Q(self):
        return self._im.Q()

    def E_step(self):
        return self._im.E_step()

    def loglik(self):
        return self._im.loglik()

    def precond(self):
        return self.model.precond

    @property
    def model(self):
        return self._model

    @model.setter
    def model(self, model):
        self._model = model
        self._im.model = model

    @property
    def mu(self):
        return self._mu

    @mu.setter
    def mu(self, mu):
        self._mu = mu
        self._im.mu = mu

    @property
    def rho(self):
        return self._rho

    @rho.setter
    def rho(self, rho):
        self._rho = rho
        self._im.rho = rho

    def dump(self, fn):
        er = EstimationResult()
        for attr in ['model', 'N0']:
            setattr(er, attr, getattr(self, "_" + attr))
        er.dump(fn)
