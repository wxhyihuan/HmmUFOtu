/*
 * PhyloTreeUnrooted.cpp
 *
 *  Created on: Dec 1, 2016
 *      Author: zhengqi
 */

#include <sstream>
#include <stack>
#include <boost/unordered_set.hpp>
#include <boost/unordered_map.hpp>
#include <cfloat>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <cassert>
#include <boost/algorithm/string.hpp> /* for boost string split */
#include "HmmUFOtuConst.h"
#include "ProgLog.h"
#include "StringUtils.h"
#include "PhyloTreeUnrooted.h"
#include "DNASubModelFactory.h"

namespace EGriceLab {
using namespace std;
using namespace EGriceLab;
using Eigen::Map;
using Eigen::Matrix4Xd;

const double PhyloTreeUnrooted::MIN_LOGLIK_EXP = DBL_MIN_EXP / 2; /* use half of the DBL_MIN_EXP to avoid numeric-underflow */
const double PhyloTreeUnrooted::INVALID_LOGLIK = 1;
//const double PhyloTreeUnrooted::LOGLIK_REL_EPS = 1e-6;
const double PhyloTreeUnrooted::BRANCH_EPS = 1e-5;

const string PhyloTreeUnrooted::KINDOM_PREFIX = "k__";
const string PhyloTreeUnrooted::PHYLUM_PREFIX = "p__";
const string PhyloTreeUnrooted::CLASS_PREFIX = "c__";
const string PhyloTreeUnrooted::ORDER_PREFIX = "o__";
const string PhyloTreeUnrooted::FAMILY_PREFIX = "f__";
const string PhyloTreeUnrooted::GENUS_PREFIX = "g__";
const string PhyloTreeUnrooted::SPECIES_PREFIX = "s__";

static const char* TAXA_SEP = ";: "; /* valid taxa name separator */

bool PTUnrooted::isTip(const PTUNodePtr& node) {
	if(node->isLeaf())
		return false;
	for(vector<PTUNodePtr>::const_iterator child = node->neighbors.begin(); child != node->neighbors.end(); ++child)
		if(isChild(*child, node) && !(*child)->isLeaf())
			return false;
	return true;
}

istream& PhyloTreeUnrooted::PhyloTreeUnrootedNode::load(istream& in) {
	/* read length */
	size_t nName, nAnno;
	in.read((char*) &nName, sizeof(string::size_type));
	in.read((char*) &nAnno, sizeof(string::size_type));

	/* read basic info */
	in.read((char*) &id, sizeof(long));
	StringUtils::loadString(name, in, nName);

	/* read seq */
	seq.load(in);

	/* read annotation */
	StringUtils::loadString(anno, in, nAnno);
	in.read((char*) &annoDist, sizeof(double));

	return in;
}

ostream& PhyloTreeUnrooted::PhyloTreeUnrootedNode::save(ostream& out) const {
	/* write length */
	size_t nName = name.length();
	size_t nAnno = anno.length();
	out.write((const char*) &nName, sizeof(size_t));
	out.write((const char*) &nAnno, sizeof(size_t));

	/* write basic info */
	out.write((const char*) &id, sizeof(long));
	StringUtils::saveString(name, out);

	/* write seq */
	seq.save(out);

	/* write annotation */
	StringUtils::saveString(anno, out);
	out.write((const char*) &annoDist, sizeof(double));

	return out;
}


PhyloTreeUnrooted::PhyloTreeUnrooted(const NewickTree& ntree) : csLen(0) {
	/* construct PTUNode by DFS of the NewickTree */
	boost::unordered_set<const NT*> visited;
	stack<const NT*> S;
	long id = 0; /* id start from 0 */
	unordered_map<const NT*, PTUNodePtr> nTree2PTree;

	S.push(&ntree);
	while(!S.empty()) {
		const NT* v = S.top();
		S.pop();
		if(visited.find(v) == visited.end()) { /* not visited before */
			visited.insert(v);
			/* construct this PTUNode */
			PTUNodePtr u = boost::make_shared<PTUNode>(id++, v->name);

			id2node.push_back(u);
			nTree2PTree[v] = u;

			/* add check each child of v */
			for(vector<NT>::const_iterator child = v->children.begin(); child != v->children.end(); ++child)
				S.push(&*child);
		}
	}

	/* explore the nTree again to establish the parent/child relationship */
	visited.clear();
	S.push(&ntree);
	while(!S.empty()) {
		const NT* v = S.top();
		S.pop();
		if(visited.find(v) == visited.end()) { /* not visited before */
			visited.insert(v);
			/* get corresponding PTUNode */
			const PTUNodePtr& u = nTree2PTree[v];
			if(root == NULL) // root node of the Newick tree encountered
				root = u;

			/* add check each child of u */
			for(vector<NT>::const_iterator Nchild = v->children.begin(); Nchild != v->children.end(); ++Nchild) {
				const PTUNodePtr& Pchild = nTree2PTree[&*Nchild];
				/* add this new edge */
				addEdge(u, Pchild);
				/* set parent */
				Pchild->parent = u;
				/* update branch length */
				setBranchLength(u, Pchild, Nchild->length);
				S.push(&*Nchild);
			}
		}
	}
	assert(root != NULL);
}

unsigned PhyloTreeUnrooted::loadMSA(const MSA& msa) {
	unsigned n0 = msaId2node.size(); /* original number of loaded nodes */
	if(msa.getAbc()->getAlias() != "DNA") {
		cerr << "PhyloTreeUnrooted can only read in MSA in DNA alphabet" << endl;
		return -1;
	}
	const unsigned numSeq = msa.getNumSeq();
	csLen = msa.getCSLen();

	/* check uniqueness of seq names in msa */
	unordered_map<string, unsigned> name2msaId;
	for(unsigned i = 0; i < numSeq; ++i) {
		string name = msa.seqNameAt(i);
		if(name2msaId.find(name) != name2msaId.end()) {
			cerr << "Non-unique seq name " << name << " found in your MSA data " << msa.getName() << endl;
			return -1;
		}
		else
			name2msaId[name] = i;
	}

	/* assign seq to each nodes of the tree, ignore nodes cannot be found (unnamed, etc) */
	for(vector<PTUNodePtr>::iterator node = id2node.begin(); node != id2node.end(); ++node) {
		assert(node - id2node.begin() == (*node)->id);

		unordered_map<string, unsigned>::const_iterator result = name2msaId.find((*node)->name);
		if(result == name2msaId.end()) /* this name cannot be found in the msa */
			continue;
		(*node)->seq = msa.dsAt(result->second);
		msaId2node[result->second] = *node;
	}
	return msaId2node.size() - n0;
}

istream& PTUnrooted::loadAnnotation(istream& in) {
	string line, name, anno;
	unordered_map<string, string> name2anno;
	while(getline(in, line)) {
		istringstream lineIn(line);
		std::getline(lineIn, name, ANNO_FIELD_SEP);
		std::getline(lineIn, anno, ANNO_FIELD_SEP);
		name2anno[name] = anno;
	}

	for(vector<PTUNodePtr>::const_iterator node = id2node.begin(); node != id2node.end(); ++node) {
		unordered_map<string, string>::const_iterator result = name2anno.find((*node)->name);
		if(result != name2anno.end())
			(*node)->name = result->second;
	}

	return in;
}

PhyloTreeUnrooted::PTUNodePtr PhyloTreeUnrooted::setRoot(const PTUNodePtr& newRoot) {
	if(newRoot == NULL || newRoot == root) /* no need to set */
		return root;

	newRoot->parent = NULL; // root has no parent
//	node2loglik[newRoot][NULL] = Matrix4Xd::Constant(4, csLen, inf); // new cache for dummy branch
	/* DFS of this tree starting from newRoot */
	boost::unordered_set<PTUNodePtr> visited;
	stack<PTUNodePtr> S;

	S.push(newRoot);
	while(!S.empty()) {
		PTUNodePtr u = S.top();
		S.pop();
		if(visited.find(u) == visited.end()) { /* not visited before */
			visited.insert(u);

			/* check each neighbor of v */
			for(vector<PTUNodePtr>::iterator v = u->neighbors.begin(); v != u->neighbors.end(); ++v) {
				if(visited.find(*v) == visited.end() /* not parent/ancestor of v */
					&& !isChild(*v, u)) /* update this child's parent */
					(*v)->parent = u;
				S.push(*v);
			}
		}
	}
	PTUNodePtr oldRoot = root;
	root = newRoot;
	return oldRoot;
}

void PhyloTreeUnrooted::resetLoglik() {
	for(vector<PTUNodePtr>::iterator u = id2node.begin(); u != id2node.end(); ++u)
		for(vector<PTUNodePtr>::iterator v = (*u)->neighbors.begin(); v != (*u)->neighbors.end(); ++v)
			node2branch[*u][*v].loglik.setConstant(INVALID_LOGLIK);
}

void PhyloTreeUnrooted::initInLoglik() {
	for(vector<PTUNodePtr>::iterator u = id2node.begin(); u != id2node.end(); ++u)
		for(vector<PTUNodePtr>::iterator v = (*u)->neighbors.begin(); v != (*u)->neighbors.end(); ++v) /* u->neighbors */
			node2branch[*u][*v].loglik = Matrix4Xd::Constant(4, csLen, INVALID_LOGLIK);
}

void PhyloTreeUnrooted::initLeafLoglik() {
	leafLoglik.resize(4, 5);
	if(model == NULL) /* no model provided yet */
		leafLoglik.setConstant(INVALID_LOGLIK);
	else {
		/* initiate the non-gap (0..3) columns */
		leafLoglik.leftCols(4).setConstant(infV);
		leafLoglik.leftCols(4).diagonal().setConstant(0);
		leafLoglik.col(4) = model->getPi().array().log();
	}
}

Vector4d PhyloTreeUnrooted::loglik(const PTUNodePtr& node, int j, double r) {
	Vector4d loglikVec = Vector4d::Zero();
	for(vector<PTUNodePtr>::const_iterator child = node->neighbors.begin(); child != node->neighbors.end(); ++child) {
		if(isChild(*child, node)) /* a child neighbor */
			loglikVec += dot_product_scaled(model->Pr(getBranchLength(node, *child) * r), loglik(*child, j)); /* evaluate child recursively */
	}
	if(node->isLeaf() && !node->seq.empty()) /* is a leaf root with assigned seq */
		loglikVec += node->seq[j] >= 0 ? leafLoglik.col(node->seq[j]) /* a base observed */ : leafLoglik.col(4) /* a gap observed */;
	return loglikVec;
}

Vector4d PhyloTreeUnrooted::loglik(const PTUNodePtr& node, int j) {
	if(isEvaluated(node, node->parent, j))
		return getBranchLoglik(node, node->parent, j);

	Vector4d loglikVec;
	if(dG == NULL)
		loglikVec = loglik(node, j, 1); // using fixed rate
	else {
		Matrix4Xd loglikMat(4, dG->getK());
		for(int k = 0; k < dG->getK(); ++k)
			loglikMat.col(k) = loglik(node, j, dG->rate(k));
		loglikVec = row_mean_exp_scaled(loglikMat); // use average of DiscreteGammaModel rate
	}
	/* cache this conditional loglik for non-root node */
	if(!node->isRoot())
		setBranchLoglik(node, node->parent, j, loglikVec);
	return loglikVec;
}

Matrix4Xd PTUnrooted::loglik(const PTUNodePtr& node) {
	if(isEvaluated(node, node->parent)) /* not a root and evaluated */
		return getBranchLoglik(node, node->parent);

	Matrix4Xd loglikMat(4, csLen);
	for(int j = 0; j < csLen; ++j)
		loglikMat.col(j) = loglik(node, j);
	if(!node->isRoot())
		setBranchLoglik(node, node->parent, loglikMat);
	return loglikMat;
}

double PTUnrooted::treeLoglik(const PTUNodePtr& node, int start, int end) {
	double loglik = 0;
	for(int j = start; j <= end; ++j)
		loglik += treeLoglik(node, j);
	return loglik;
}

int8_t PhyloTreeUnrooted::inferState(const PTUnrooted::PTUNodePtr& node, int j) {
	if(!node->seq.empty())
		return node->seq[j];
	Vector4d logV = loglik(node, j);
	int8_t state;
	logV.maxCoeff(&state);
	return state;
}

void PhyloTreeUnrooted::evaluate(const PTUNodePtr& node, int j) {
	if(isEvaluated(node, node->parent, j)) /* already evaluated */
		return;
	for(vector<PTUNodePtr>::const_iterator child = node->neighbors.begin(); child != node->neighbors.end(); ++child) { /* check each child */
		if(isChild(*child, node)) /* a child neighbor */
			loglik(*child, j); /* evaluate child recursively */
	}
}

ostream& PTUnrooted::writeTreeNewick(ostream& out, const PTUNodePtr& node) const {
	bool first = true;
	if(node->isRoot() || node->isInternal()) {
		out << '(';
		for(std::vector<PTUNodePtr>::const_iterator child = node->neighbors.begin(); child != node->neighbors.end(); ++child) {
			if(!isChild(*child, node)) /* not a child */
				continue;
			out << (first ? "" : ",");
			writeTreeNewick(out, *child);
			first = false;
		}
		out << ')';
	}
	if(StringUtils::containsWhiteSpace(node->name) || StringUtils::containsAny(node->name, NewickTree::INVALID_CHARS)) // name contains INVALID CHARS
		out << "'" << node->name << "'";
	else
		out << node->name;
	double length = getBranchLength(node, node->parent);
	if(length > 0)
		out << ':' << length;

	return out;
}

vector<Matrix4d> PTUnrooted::getModelTraningSetGoldman() const {
	debugLog << "Training data using Gojobori method" << endl;
	vector<Matrix4d> data; // store observed base transition counts
	/* check every node of this tree */
	for(vector<PTUNodePtr>::const_iterator node = id2node.begin(); node != id2node.end(); ++node) {
		if((*node)->isTip() && (*node)->neighbors.size() > 2) { // tip with >=2 children
//			cerr << "Find a candidate node id: " << (*node)->id << endl;
//			cerr << "First child: " << (*node)->firstChild()->name << endl;
//			cerr << "Last child: " << (*node)->lastChild()->name << endl;
			const DigitalSeq& seq1 = (*node)->firstChild()->seq;
			const DigitalSeq& seq2 = (*node)->lastChild()->seq;
			if(DNASubModel::pDist(seq1, seq1) <= DNASubModel::MAX_PDIST)
				data.push_back(DNASubModel::calcTransFreq2Seq(seq1, seq2));
		}
	}
	return data;
}

vector<Matrix4d> PTUnrooted::getModelTraningSetGojobori() const {
	vector<Matrix4d> data; // store observed base transition counts
	/* check every node of this tree */
	for(vector<PTUNodePtr>::const_iterator node = id2node.begin(); node != id2node.end(); ++node) {
		const vector<PTUNodePtr> children = (*node)->getChildren();
		if(children.size() == 2 &&
				(children.front()->isTip() || children.back()->isTip()) ) { /* one child is a tip node */
//			cerr << "Find a candidate node id: " << (*node)->id << endl;
//			cerr << "Child 1 is tip " << children.front()->isTip() << endl;
//			cerr << "Child 2 is tip " << children.back()->isTip() << endl;
			PTUNodePtr tipChild = children.front();
			PTUNodePtr outerChild = children.back();
			if(!tipChild->isTip())
				tipChild.swap(outerChild);

			const DigitalSeq& seq0 = PTUnrooted::randomLeaf(outerChild)->seq;
			const DigitalSeq& seq1 = tipChild->firstChild()->seq;
			const DigitalSeq& seq2 = tipChild->lastChild()->seq;
			if(DNASubModel::pDist(seq0, seq1) <= DNASubModel::MAX_PDIST &&
					DNASubModel::pDist(seq0, seq2) <= DNASubModel::MAX_PDIST)
								data.push_back(DNASubModel::calcTransFreq3Seq(seq0, seq1, seq2));
		}
	}
	debugLog << "Gojobori data prepared" << endl;
	return data;
}

Vector4d PTUnrooted::getModelFreqEst() const {
	Vector4d freq = Vector4d::Zero();
	for(vector<PTUNodePtr>::const_iterator node = id2node.begin(); node != id2node.end(); ++node)
		if((*node)->isLeaf())
			freq += DNASubModel::calcBaseFreq((*node)->seq);
	return freq;
}

istream& PTUnrooted::load(istream& in) {
//	initInLoglik();
//	initLeafLoglik();
	/* Read program info */
	string pname, pver;
	readProgName(in, pname);
	if(pname != progName) {
		errorLog << "Not a PTUnrooted object file" << endl;
		in.setstate(ios_base::failbit);
		return in;
	}
	readProgVersion(in, pver);
	if(cmpVersion(progVersion, pver) < 0) {
		errorLog << "You are trying using an older version " << (progName + progVersion) <<
				" to read a newer PTUnrooted data file that was build by " << (pname + pver) << endl;
		in.setstate(ios_base::failbit);
		return in;
	}

	/* read global information */
	size_t nNodes;
	in.read((char*) &nNodes, sizeof(size_t));
	in.read((char*) &csLen, sizeof(int));

	/* read each node */
	for(size_t i = 0; i < nNodes; ++i) {
		PTUNodePtr node(new PTUNode); /* construct a new node */
		node->load(in);
		id2node.push_back(node);
	}

	/* read all edges */
	size_t nEdges;
	in.read((char*) &nEdges, sizeof(size_t));
	for(size_t i = 0; i < nEdges; ++i)
		loadEdge(in);

	/* read leaf loglik */
	loadLeafLoglik(in);

	/* load root */
	loadRoot(in);

	/* load root loglik */
//	loadRootLoglik(in);

	/* read index */
	loadMSAIndex(in);

	/* load models */
	loadModel(in);
	loadDGModel(in);

	return in;
}

ostream& PTUnrooted::save(ostream& out) const {
	/* save program info */
	writeProgName(out, progName);
	writeProgVersion(out, progVersion);

	/* write global information */
	size_t nNodes = numNodes();
	out.write((const char*) &nNodes, sizeof(size_t));
	out.write((const char*) &csLen, sizeof(int));

	/* write each node */
	for(vector<PTUNodePtr>::const_iterator node = id2node.begin(); node != id2node.end(); ++node)
		(*node)->save(out);
	/* write all edges */
	size_t nEdges = numEdges();
	out.write((const char*) &nEdges, sizeof(size_t));
	for(vector<PTUNodePtr>::const_iterator u = id2node.begin(); u != id2node.end(); ++u)
		for(vector<PTUNodePtr>::const_iterator v = (*u)->neighbors.begin(); v != (*u)->neighbors.end(); ++v)
			saveEdge(out, *u, *v);

	/* write leaf loglik */
	saveLeafLoglik(out);

	/* save root */
	saveRoot(out);

	/* write index */
	saveMSAIndex(out);

	/* save models */
	saveModel(out);
	saveDGModel(out);

	return out;
}

ostream& PTUnrooted::saveMSAIndex(ostream& out) const {
	unsigned N = msaId2node.size();
	out.write((const char*) &N, sizeof(unsigned));
	for(map<unsigned, PTUNodePtr>::const_iterator it = msaId2node.begin(); it != msaId2node.end(); ++it) {
		out.write((const char*) &(it->first), sizeof(unsigned));
		out.write((const char*) &(it->second->id), sizeof(long));
	}

	return out;
}

istream& PTUnrooted::loadMSAIndex(istream& in) {
	unsigned N = 0;
	unsigned msaId;
	long id;
	in.read((char*) &N, sizeof(unsigned));
	for(unsigned i = 0; i < N; ++i) {
		in.read((char*) &msaId, sizeof(unsigned));
		in.read((char*) &id, sizeof(long));
		msaId2node[msaId] = id2node.at(id); /* build index */
	}

	return in;
}

ostream& PTUnrooted::saveEdge(ostream& out, const PTUNodePtr& node1, const PTUNodePtr& node2) const {
	out.write((const char*) &(node1->id), sizeof(long));
	out.write((const char*) &(node2->id), sizeof(long));
	bool flag = isParent(node1, node2);
	out.write((const char*) &flag, sizeof(bool));
	getBranch(node1, node2).save(out); /* save branch data */

	return out;
}

istream& PTUnrooted::loadEdge(istream& in) {
	long id1, id2;
	bool isParent;
	in.read((char*) &id1, sizeof(long));
	in.read((char*) &id2, sizeof(long));
	in.read((char*) &isParent, sizeof(bool));

	const PTUNodePtr& node1 = id2node[id1];
	const PTUNodePtr& node2 = id2node[id2];
	node1->neighbors.push_back(node2);
	if(isParent)
		node2->parent = node1;
	/* construct a new empty branch and load */
	node2branch[node1][node2].load(in);

	return in;
}

istream& PTUnrooted::loadLeafLoglik(istream& in) {
	leafLoglik.resize(4, 5);
	double* buf = new double[leafLoglik.size()];
	in.read((char*) buf, leafLoglik.size() * sizeof(double));
	leafLoglik = Map<Matrix4Xd>(buf, 4, 5); /* copy via Map */
	delete[] buf;
	return in;
}

ostream& PTUnrooted::saveLeafLoglik(ostream& out) const {
	double* buf = new double[leafLoglik.size()];
	Map<Matrix4Xd> leafLoglikMap(buf, leafLoglik.rows(), leafLoglik.cols());
	leafLoglikMap = leafLoglik; /* copy via Map */
	out.write((const char*) buf, leafLoglik.size() * sizeof(double));
	delete[] buf;
	return out;
}

istream& PTUnrooted::loadRoot(istream& in) {
	/* set current root */
	long rootId;
	in.read((char*) &rootId, sizeof(long));
	root = id2node[rootId];

	return in;
}

ostream& PTUnrooted::saveRoot(ostream& out) const {
	/* save current root id */
	out.write((const char*) &(root->id), sizeof(long));
	return out;
}

istream& PTUnrooted::loadRootLoglik(istream& in) {
	long id;
	double* buf = new double[4 * csLen];
	Map<Matrix4Xd> inLoglikMap(buf, 4, csLen);

	for(vector<PTUNodePtr>::const_iterator node = id2node.begin(); node != id2node.end(); ++node) {
		in.read((char*) &id, sizeof(long));
		assert(id == (*node)->id);
		in.read((char*) buf, 4 * csLen * sizeof(double));
		/* assign this loglik */
		setBranchLoglik(*node, NULL, inLoglikMap);
	}
	delete[] buf;

	return in;
}

ostream& PTUnrooted::saveRootLoglik(ostream& out) const {
	double* buf = new double[4 * csLen];
	Map<Matrix4Xd> inLoglikMap(buf, 4, csLen);

	for(vector<PTUNodePtr>::const_iterator node = id2node.begin(); node != id2node.end(); ++node) {
		out.write((const char*) &((*node)->id), sizeof(long));
		inLoglikMap = getBranchLoglik(*node, NULL); /* copy node->NULL loglik */
		out.write((const char*) buf, inLoglikMap.size() * sizeof(double));
	}
	delete[] buf;

	return out;
}

istream& PTUnrooted::loadModel(istream& in) {
	string type, line;
	in >> type;
	in.ignore(); /* ignore the next '\n' character */
	/* create the model with a newly created object */
	model.reset(DNASubModelFactory::createModel(type));
	/* read model */
	in >> *model;
	return in;
}

ostream& PTUnrooted::saveModel(ostream& out) const {
	out << model->modelType() << endl;
	out << *model;
	return out;
}

istream& PTUnrooted::loadDGModel(istream& in) {
	bool modelSet;
	in.read((char*) &modelSet, sizeof(bool));
	if(modelSet) {
		dG.reset(new DiscreteGammaModel()); /* construct a new model and assign to dG */
		dG->load(in);
	}
	return in;
}

ostream& PTUnrooted::saveDGModel(ostream& out) const {
	bool modelSet = dG != NULL;
	out.write((const char*) &modelSet, sizeof(bool));
	if(modelSet)
		dG->save(out);
	return out;
}

PTUnrooted PTUnrooted::copySubTree(const PTUNodePtr& u, const PTUNodePtr& v) const {
	assert(isParent(v, u));

	PTUnrooted tree; /* construct an empty tree */
	long id = 0;
	tree.csLen = csLen; /* copy csLen */
	tree.model = model; /* copy the DNA model */
	tree.dG = dG; /* copy DiscreteGammaModel */
	tree.leafLoglik = leafLoglik; /* copy leaf loglik */

	/* construct new  of nodes */
	PTUNodePtr v2(new PTUNode(id++, v->name, v->seq, v->anno, v->annoDist));
	PTUNodePtr u2(new PTUNode(id++, u->name, u->seq, u->anno, u->annoDist));
	u2->parent = v2;

	/* add nodes */
	tree.id2node.push_back(v2);
	tree.id2node.push_back(u2);
	/* add edge */
	tree.addEdge(u2, v2);

	/* copy branch length and loglik */
	tree.setBranch(u2, v2, getBranch(u, v));
	tree.setBranch(v2, u2, getBranch(v, u));

	tree.setRoot(v2);
	return tree;
}

double PTUnrooted::estimateBranchLength(const PTUNodePtr& u, const PTUNodePtr& v, int start, int end) const {
	assert(isParent(v, u));
	const Vector4d& pi = model->getPi();

	const Matrix4Xd& loglikU = getBranchLoglik(u, v);
	const Matrix4Xd& loglikV = getBranchLoglik(v, u);

	double p = 0;
	int N = 0;
	for(int j = start; j <= end; ++j) {
		double logA = dot_product_scaled(pi, loglikU.col(j) + loglikV.col(j));
		double logB = dot_product_scaled(pi, loglikU.col(j)) + dot_product_scaled(pi, loglikV.col(j));
		if(::isnan(logA) || ::isnan(logB))
			continue;
		double scale = std::max(logA, logB);
		logA -= scale;
		logB -= scale;
		p += ::exp(logB) / (::exp(logA) + ::exp(logB));
		N++;
//		fprintf(stderr, "logA:%g logB:%g scale:%g p:%g\n", logA, logB, scale, p);
	}
	return p / N;
}

double PTUnrooted::optimizeBranchLength(const PTUNodePtr& u, const PTUNodePtr& v,
		int start, int end) {
	assert(isParent(v, u));

	double w0 = estimateBranchLength(u, v);

	double q0 = ::exp(-w0);
	double p0 = 1 - q0;

	double p = p0;
	double q = q0;

	const Vector4d& pi = model->getPi();

	const Matrix4Xd& loglikU = getBranchLoglik(u, v);
	const Matrix4Xd& loglikV = getBranchLoglik(v, u);
	/* Felsenstein's iterative optimizing algorithm */
	while(p >= 0 && p <= 1) {
		p = 0;
		int N = 0;
		for(int j = start; j <= end; ++j) {
			double logA = dot_product_scaled(pi, loglikU.col(j) + loglikV.col(j));
			double logB = dot_product_scaled(pi, loglikU.col(j)) + dot_product_scaled(pi, loglikV.col(j));
			if(::isnan(logA) || ::isnan(logB))
				continue;
			double scale = std::max(logA, logB);
			logA -= scale;
			logB -= scale;
			p += ::exp(logB) * p0 / (::exp(logA) * q0 + ::exp(logB) * p0);
			N++;
		}
		p /= N;
		q = 1 - p;

		if(::fabs(::log(q) - ::log(q0)) < BRANCH_EPS)
			break;
		// update p0 and q0
		p0 = p;
		q0 = q;
	}

	double w = -::log(q); // final estimation
	setBranchLength(u, v, w);

	return w;
}

double PTUnrooted::optimizeBranchLength(const PTUNodePtr& u, const PTUNodePtr& v, double maxL,
		int start, int end) {
	assert(isParent(v, u));

	double w0 = getBranchLength(u, v);

	double q0 = ::exp(-w0);
	double p0 = 1 - q0;

	double p = p0;
	double q = q0;

	const Vector4d& pi = model->getPi();

	const Matrix4Xd& loglikU = getBranchLoglik(u, v);
	const Matrix4Xd& loglikV = getBranchLoglik(v, u);
	/* Felsenstein's iterative optimizing algorithm */
	while(p >= 0 && p <= 1) {
		p = 0;
		int N = 0;
		for(int j = start; j <= end; ++j) {
			double logA = dot_product_scaled(pi, loglikU.col(j) + loglikV.col(j));
			double logB = dot_product_scaled(pi, loglikU.col(j)) + dot_product_scaled(pi, loglikV.col(j));
			if(::isnan(logA) || ::isnan(logB))
				continue;
			double scale = std::max(logA, logB);
			logA -= scale;
			logB -= scale;
			p += ::exp(logB) * p0 / (::exp(logA) * q0 + ::exp(logB) * p0);
			N++;
		}
		p /= N;
		q = 1 - p;

		if(::fabs(q - q0) < BRANCH_EPS)
			break;
		// update p0 and q0
		p0 = p;
		q0 = q;
	}

	double w = -::log(q); // final estimation
	if(w > maxL) // restrain
		w = maxL;
	setBranchLength(u, v, w);

	return w;
}

double PTUnrooted::optimizeBranchLength(const PTUNodePtr& u, const PTUNodePtr& r, const PTUNodePtr& v,
		int start, int end, bool doUpdate) {
	assert(isParent(r, u) && isParent(v, r));
	PTUNodePtr oldRoot = root;

	double wur0 = getBranchLength(u, r);
	double wvr0 = getBranchLength(v, r);
	double w0 = wur0 + wvr0;

	double wur = wur0;
	double wvr = wvr0;

	/* wur depend on loglik(r,u) on wvr */
	/* wvr depend on loglik(r,v) on wur */
	while(0 <= wur && wur <= w0) {
		setRoot(r);
		/* wur -> loglik(r,v) */

		wur = optimizeBranchLength(u, r, w0, start, end);

		if(doUpdate) { /* keep all loglik updated */
			resetLoglik(r, v);
			setRoot(v);
			evaluate();
		}

		/* wvr -> loglik(r,u) */
		wvr = w0 - wur;
		setBranchLength(v, r, wvr);

		setRoot(r);
		resetLoglik(r, u);
		setRoot(u);
		evaluate();

		if(::fabs(wur - wur0) < BRANCH_EPS)
			break;

		wur0 = wur;
	}
	setRoot(oldRoot);

	return wur / w0;
}

double PTUnrooted::estimateSeq(const DigitalSeq& seq, const PTUNodePtr& u, const PTUNodePtr& v,
		int start, int end) const {
	assert(seq.length() == csLen);
	assert(isParent(v, u));

	double w = getBranchLength(u, v);
	double treeLik = 0;
	for(int j = start; j <= end; ++j) {
		Vector4d loglik = dot_product_scaled(model->Pr(w / 2), getBranchLoglik(u, v, j))  /* u->v convoluting to w/2 */
						+ dot_product_scaled(model->Pr(w / 2), getBranchLoglik(v, u, j)); /* v->u convoluting to w/2 */
		loglik += seq[j] >= 0 ? leafLoglik.col(seq[j]) /* a base observed */ : leafLoglik.col(4) /* a gap observed */;
		treeLik += dot_product_scaled(model->getPi(), loglik);
	}
	return treeLik;
}

double PTUnrooted::placeSeq(const DigitalSeq& seq, const PTUNodePtr& u, const PTUNodePtr& v,
		int start, int end) {
	assert(seq.length() == csLen); /* make sure this is an aligned seq */
	assert(isParent(v, u));

	/* break the connection of u and v */
	double w0 = getBranchLength(u, v);
	removeEdge(u, v);
	/* create a new interior root */
	PTUNodePtr r(new PTUNode(numNodes(), v->name, csLen));
	/* create a new leaf with given seq */
	PTUNodePtr n(new PTUNode(numNodes() + 1, v->name, seq));
	n->parent = r;
	u->parent = r;
	r->parent = v;
	/* add new nodes */
	id2node.push_back(r);
	id2node.push_back(n);
	/* place r at the middle-point of u and v */
	addEdge(u, r);
	addEdge(v, r);
	setBranch(u, r, getBranch(u, v));
	setBranch(v, r, getBranch(v, u));
	assert(isEvaluated(u, r));
	assert(isEvaluated(v, r));
	setBranchLength(u, r, w0 * 0.5);
	setBranchLength(v, r, w0 * 0.5);
	setBranchLoglik(r, u, Matrix4Xd::Constant(4, csLen, INVALID_LOGLIK));
	setBranchLoglik(r, v, Matrix4Xd::Constant(4, csLen, INVALID_LOGLIK));
	/* place r with un-initial branch length (0) */
	addEdge(n, r);
	setBranchLoglik(r, n, Matrix4Xd::Constant(4, csLen, INVALID_LOGLIK));
	setBranchLoglik(n, r, Matrix4Xd::Constant(4, csLen, INVALID_LOGLIK));

	/* evaluate and optimize the new branches */
	setRoot(n);
	evaluate(); /* r->n evaluated */
	setRoot(r);
	evaluate(); /* n->r evaluated */
	optimizeBranchLength(n, r, start, end); /* n->r length optimized */
	setRoot(u);
	evaluate(); /* r->u evaluated */
	setRoot(v);
	evaluate(); /* r->v evaluated */

	optimizeBranchLength(u, r, v, start, end); /* r->u and r->v optimized jointly */
	setRoot(r);

	return treeLoglik(start, end);
}

bool PhyloTreeUnrooted::isFullCanonicalName(const string& taxa) {
	vector<string> fields;
	boost::split(fields, taxa, boost::is_any_of(TAXA_SEP), boost::token_compress_on);
	for(vector<string>::size_type level = 0; level < fields.size(); ++level)
		if(!isCanonicalName(fields[level], static_cast<TaxaLevel> (level)))
			return false;
	return true;
}

bool PhyloTreeUnrooted::isPartialCanonicalName(const string& taxa) {
	vector<string> fields;
	boost::split(fields, taxa, boost::is_any_of(TAXA_SEP), boost::token_compress_on);
	for(vector<string>::const_iterator name = fields.begin(); name != fields.end(); ++name)
		if(!isCanonicalName(*name))
			return false;
	return true;
}

string PhyloTreeUnrooted::formatTaxaName(const string& taxa) {
	if(taxa.empty())
		return taxa;

	vector<string> formatedTaxa;
	vector<string> fields;
	boost::split(fields, taxa, boost::is_any_of(TAXA_SEP), boost::token_compress_on);
	for(vector<string>::const_iterator name = fields.begin(); name != fields.end(); ++name)
		if(isCanonicalName(*name))
			formatedTaxa.push_back(*name);

	return boost::join(formatedTaxa, ";");
}

vector<PTUnrooted::PTUNodePtr> PTUnrooted::getLeafHitsByPDist(const DigitalSeq& seq, double maxDist,
		int start, int end) const {
	vector<PTUnrooted::PTUNodePtr> hits;
	for(vector<PTUnrooted::PTUNodePtr>::const_iterator node = id2node.begin(); node != id2node.end(); ++node) {
		if((*node)->isLeaf() && DNASubModel::pDist((*node)->seq, seq, start, end) <= maxDist)
			hits.push_back(*node);
	}
	return hits;
}

vector<PTUnrooted::PTUNodePtr> PTUnrooted::getLeafHitsByPDist(const vector<PTUNodePtr>& candidates, const DigitalSeq& seq,
		double maxDist, int start, int end) const {
	if(candidates.empty()) /* no candidates provided */
		return getLeafHitsByPDist(seq, maxDist, start, end);

	vector<PTUnrooted::PTUNodePtr> hits;
	for(vector<PTUnrooted::PTUNodePtr>::const_iterator node = candidates.begin(); node != candidates.end(); ++node) {
		if((*node)->isLeaf() && DNASubModel::pDist((*node)->seq, seq, start, end) <= maxDist)
			hits.push_back(*node);
	}
	return hits;
}

vector<PTUnrooted::PTUNodePtr> PTUnrooted::getLeafHitsBySubDist(const DigitalSeq& seq, double maxDist,
		int start, int end) const {
	vector<PTUnrooted::PTUNodePtr> hits;
	for(vector<PTUnrooted::PTUNodePtr>::const_iterator node = id2node.begin(); node != id2node.end(); ++node) {
		if((*node)->isLeaf() && model->subDist((*node)->seq, seq, start, end) <= maxDist)
			hits.push_back(*node);
	}
	return hits;
}

vector<PTUnrooted::PTUNodePtr> PTUnrooted::getLeafHitsBySubDist(const vector<PTUNodePtr>& candidates, const DigitalSeq& seq,
		double maxDist, int start, int end) const {
	if(candidates.empty()) /* no candidates provided */
		return getLeafHitsBySubDist(seq, maxDist, start, end);

	vector<PTUnrooted::PTUNodePtr> hits;
	for(vector<PTUnrooted::PTUNodePtr>::const_iterator node = candidates.begin(); node != candidates.end(); ++node) {
		if((*node)->isLeaf() && model->subDist((*node)->seq, seq, start, end) <= maxDist)
			hits.push_back(*node);
	}
	return hits;
}

void PhyloTreeUnrooted::annotate() {
	for(vector<PTUNodePtr>::const_iterator node = id2node.begin(); node != id2node.end(); ++node)
		annotate(*node);
}

void PhyloTreeUnrooted::annotate(const PTUNodePtr& node) {
	vector<string> annoPath;
	PTUNodePtr p(node); /* pointer to current node */
	while(!isFullCanonicalName(p->name) && !p->isRoot()) { /* a non-full canonical named node */
		node->annoDist += getBranchLength(p, p->parent);
		if(isPartialCanonicalName(p->name))
			annoPath.push_back(p->name);
		p = p->parent;
	}
	if(isFullCanonicalName(p->name))
		annoPath.push_back(p->name); /* push last name */
	std::reverse(annoPath.begin(), annoPath.end()); /* reverse the annoPath */
	node->anno = !annoPath.empty() ? boost::join(annoPath, ";") : "Other";
}

size_t PhyloTreeUnrooted::estimateNumMutations(int j) {
	size_t N = 0;
	for(vector<PTUNodePtr>::const_iterator nodeIt = id2node.begin(); nodeIt != id2node.end(); ++nodeIt) {
		if(!(*nodeIt)->isRoot() && inferState((*nodeIt), j) != inferState((*nodeIt)->parent, j)) {
			N++;
		}
	}
	return N;
}

ostream& PTUnrooted::PTUBranch::save(ostream& out) const {
	out.write((const char*) &length, sizeof(double));
	size_t N = loglik.size();
	out.write((const char*) &N, sizeof(size_t));

	double *buf = new double[N];
	Map<Matrix4Xd> loglikMap(buf, 4, loglik.cols());
	loglikMap = loglik; /* copy data */
	out.write((const char*) buf, sizeof(double) * N);
	delete[] buf;

	return out;
}

istream& PTUnrooted::PTUBranch::load(istream& in) {
	in.read((char*) &length, sizeof(double));
	size_t N;
	in.read((char*) &N, sizeof(size_t));
	if(loglik.size() != N)
		loglik.resize(4, N / 4);

	double *buf = new double[N];
	in.read((char*) buf, sizeof(double) * N);
//	Map<Matrix4Xd> loglikMap(buf, 4, N / 4);
//	loglik = loglikMap;
	loglik = Map<Matrix4Xd>(buf, 4, N / 4); /* copy data */
	delete[] buf;

	return in;
}

} /* namespace EGriceLab */


