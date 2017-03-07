/*
 * DNASubModelFactory.cpp
 *
 *  Created on: Dec 16, 2016
 *      Author: zhengqi
 */

#include "DNASubModelFactory.h"
#include "GTR.h"
#include "HKY85.h"
#include "F81.h"

namespace EGriceLab {

DNASubModel* DNASubModelFactory::createModel(const string& type) {
	if(type == "GTR")
		return new GTR();
	else if(type == "HKY85")
		return new HKY85();
	else if(type == "F81")
		return new F81();
	else
		throw std::invalid_argument("Unknown DNA Substitution Model type: " + type);
}

} /* namespace EGriceLab */


