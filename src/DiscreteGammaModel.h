/*******************************************************************************
 * This file is part of HmmUFOtu, an HMM and Phylogenetic placement
 * based tool for Ultra-fast taxonomy assignment and OTU organization
 * of microbiome sequencing data with species level accuracy.
 * Copyright (C) 2017  Qi Zheng
 *
 * HmmUFOtu is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HmmUFOtu is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with AlignerBoost.  If not, see <http://www.gnu.org/licenses/>.
 *******************************************************************************/
/*
 * DiscreteGammaModel.h
 * A Discrete-Gamma distribution model to capture the rate-heterogeinity among different sites
 *  Created on: Feb 17, 2017
 *      Author: zhengqi
 */

#ifndef SRC_DISCRETEGAMMAMODEL_H_
#define SRC_DISCRETEGAMMAMODEL_H_

#include <string>
#include <iostream>
#include <Eigen/Dense>
#include <math.h> /* C99 header */
#include "HmmUFOtuConst.h"

using Eigen::VectorXd;

namespace EGriceLab {
namespace HmmUFOtu {

class DiscreteGammaModel {
public:
	/* constructors */

	/**
	 * Default constructor
	 */
	DiscreteGammaModel() : alpha(nan), K(0) { }

	/**
	 * Construct a model with given K and alpha
	 */
	DiscreteGammaModel(int K, double alpha) : K(K), alpha(alpha) {
		b.resize(K + 1);
		r.resize(K);
		setBreaks();
		setRates();
	}

	/* member methods */
	int getK() const {
		return K;
	}

	double getShape() const {
		return alpha;
	}

	void setShape(double alpha) {
		this->alpha = alpha;
		setBreaks();
		setRates();
	}

	double rate(int k) const {
		return r(k);
	}

	const VectorXd& rate() const {
		return r;
	}

	/**
	 * load model from an input stream
	 */
	istream& load(istream& in);

	/**
	 * save this model to an output stream
	 */
	ostream& save(ostream& out) const;

	/**
	 * make a fresh heap copy of this object
	 */
	DiscreteGammaModel* clone() const {
		return new DiscreteGammaModel(*this);
	}

	/**
	 * Estimate the shape parameter (alpha) of a beta distribution given a sample using specified method
	 * @param X  iid sample of observed changes
	 * @param K  number of categories
	 * @param method  estimation method
	 * @return estimated shape parameter
	 * @return 0 if method is not recognized
	 */
	static double estimateShape(const VectorXd& X, const string& method = "moment");

	/**
	 * Estimate the shape parameter (alpha) of a beta distribution using moment matching method
	 * the observed changes should follow a negative-binomial distribution
	 * @param X  iid sample of observed changes
	 * @return estimated shape parameter
	 */
	static double estimateShapeMoment(const VectorXd& X);

	/* private member methods */
private:
	/**
	 * Set the break-points according to current alpha
	 * break k is approximated with Chi-squared distribution with df = 2*alpha (Yang 1994b)
	 * to fast speed and infinite values
	 */
	void setBreaks();

	/**
	 * Set the average rates of each category according to alpha and breaks
	 */
	void setRates();

	/* member fields */
private:
	double alpha; // shape parameter (and the scare) of the underlying gamma distribution
	int K; // number of discrete categories
	VectorXd b; // break-points to devide Gamma distribution to equal prob-K categories
	VectorXd r; // average rate of each category
};

inline double DiscreteGammaModel::estimateShape(const VectorXd& X, const string& method) {
	if(method == "moment")
		return estimateShapeMoment(X);
	else
		return 0;
}

} /* namespace HmmUFOtu */
} /* namespace EGriceLab */

#endif /* SRC_DISCRETEGAMMAMODEL_H_ */
