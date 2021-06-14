/*
 Copyright (c) 2018-2021 by Marek Wydmuch, Kalina Jasinska-Kobus

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
 */

#include <fstream>
#include <iostream>
#include <random>

#include "base.h"
#include "linear.h"
#include "online_optimization.h"
#include "log.h"
#include "misc.h"
#include "threads.h"


//TODO: Refactor base class

Base::Base() {
    lossType = logistic;
    classCount = 0;
    firstClass = 0;
    firstClassCount = 0;
    t = 0;

    W = nullptr;
    G = nullptr;
}

Base::Base(Args& args): Base(){
    if(args.optimizerType != liblinear)
        setupOnlineTraining(args);
}

Base::~Base() { clear(); }

void Base::update(double label, Feature* features, Args& args) {
    std::lock_guard<std::mutex> lock(updateMtx);

    unsafeUpdate(label, features, args);
}

void Base::unsafeUpdate(double label, Feature* features, Args& args) {
    if (args.tmax != -1 && args.tmax < t) return;

    ++t;
    if (label == firstClass) ++firstClassCount;

    double pred = predictValue(features);
    double grad;
    if(args.lossType == logistic)
        grad = logisticGrad(label, pred, 0);
    else
        grad = squaredHingeGrad(label, pred, 0);

    if (args.optimizerType == sgd)
        updateSGD(*W, *G, features, grad, t, args);
    else if (args.optimizerType == adagrad)
        updateAdaGrad(*W, *G, features, grad, t, args);
    else throw std::invalid_argument("Unknown optimizer type");

    // Check if we should change sparse W to dense W
//    if (mapW != nullptr && wSize != 0) {
//        nonZeroW = mapW->size();
//        if (mapSize() > denseSize()) toDense();
//    }
}

void Base::trainLiblinear(ProblemData& problemData, Args& args) {
    double cost = args.cost;
    if (args.autoCLog)
        cost *= 1.0 + log(static_cast<double>(problemData.r) / problemData.binFeatures.size());
    if (args.autoCLin)
        cost *= static_cast<double>(problemData.r) / problemData.binFeatures.size();

    problem P = {.l = static_cast<int>(problemData.binLabels.size()),
                 .n = problemData.n,
                 .y = problemData.binLabels.data(),
                 .x = problemData.binFeatures.data(),
                 .bias = -1,
                 .W = problemData.instancesWeights.data()};

    parameter C = {.solver_type = args.solverType,
                   .eps = args.eps,
                   .C = cost,
                   .nr_weight = problemData.labelsCount,
                   .weight_label = problemData.labels,
                   .weight = problemData.labelsWeights,
                   .p = 0,
                   .init_sol = NULL,
                   .max_iter = args.maxIter};

    auto output = check_parameter(&P, &C);
    assert(output == NULL);

    model* M = train_liblinear(&P, &C);

    assert(M->nr_class <= 2);
    assert(M->nr_feature == problemData.n);

    // Set base's attributes
    firstClass = M->label[0];
    classCount = M->nr_class;

    // Copy weights
    W = new Vector<Weight>(problemData.n + 1);
    for (int i = 0; i < problemData.n; ++i) W->insertD(i + 1, M->w[i]); // Shift by 1

    if(args.solverType == L2R_L2LOSS_SVC_DUAL || args.solverType == L2R_L2LOSS_SVC ||
        args.solverType == L2R_L1LOSS_SVC_DUAL || args.solverType == L1R_L2LOSS_SVC)
        lossType = squaredHinge;

    // Delete LibLinear model
    free_model_content(M);
    free(M);
}

void Base::trainOnline(ProblemData& problemData, Args& args) {
    delete W;
    delete G;
    classCount = 2;
    firstClass = 1;
    t = 0;

    Vector<Weight>* _W = new Vector<Weight>(problemData.n);
    Vector<Weight>* _G = nullptr;

    // Set loss function
    lossType = args.lossType;
    double (*lossFunc)(double, double, double);
    double (*gradFunc)(double, double, double);
    if (args.lossType == logistic) {
        lossFunc = &logisticLoss;
        gradFunc = &logisticGrad;
    }
    else if (args.lossType == squaredHinge) {
        gradFunc = &squaredHingeGrad;
    }
    else if (args.lossType == pwLogistic) {
        lossFunc = &pwLogisticLoss;
        gradFunc = &pwLogisticGrad;
    }
    else
        throw std::invalid_argument("Unknown loss function type");

    // Set update function
    void (*updateFunc)(Vector<Weight>&, Vector<Weight>&, Feature*, double, int, Args&);
    if(args.optimizerType == sgd) {
        updateFunc = &updateSGD;
    }
    else if (args.optimizerType == adagrad){
        updateFunc = &updateAdaGrad;
        _G = new Vector<Weight>(problemData.n);
    }
    else
        throw std::invalid_argument("Unknown online update function type");

    const int examples = problemData.binFeatures.size();
    double loss = 0;
    for (int e = 0; e < args.epochs; ++e)
        for (int r = 0; r < examples; ++r) {
            double label = problemData.binLabels[r];
            Feature* features = problemData.binFeatures[r];

            if (args.tmax != -1 && args.tmax < t) break;

            ++t;
            if (problemData.binLabels[r] == firstClass) ++firstClassCount;

            double pred = _W->dot(features);
            double grad = gradFunc(label, pred, problemData.invPs) * problemData.instancesWeights[r];
            updateFunc(*_W, *_G, features, grad, t, args);

            // Report loss
//            loss += lossFunc(label, pred, problemData.invPs);
//            int iter = e * examples + r;
//            if(iter % 10000 == 9999)
//                Log(CERR) << "  Iter: " << iter << "/" << args.epochs * examples << ", loss: " << loss / iter << "\n";
        }

    W = _W;
    G = _G;
    //finalizeOnlineTraining(args);
}

void Base::train(ProblemData& problemData, Args& args) {

    if (problemData.binLabels.empty()) {
        firstClass = 0;
        classCount = 0;
        return;
    }

    assert(problemData.binLabels.size() == problemData.binFeatures.size());
    assert(problemData.instancesWeights.size() >= problemData.binLabels.size());

    int positiveLabels = std::count(problemData.binLabels.begin(), problemData.binLabels.end(), 1.0);
    if (positiveLabels == 0 || positiveLabels == problemData.binLabels.size()) {
        firstClass = static_cast<int>(problemData.binLabels[0]);
        classCount = 1;
        return;
    }

    // Apply some weighting for very unbalanced data
    if (args.inbalanceLabelsWeighting) {
        problemData.labelsCount = 2;
        problemData.labels = new int[2];
        problemData.labels[0] = 0;
        problemData.labels[1] = 1;
        problemData.labelsWeights = new double[2];

        int negativeLabels = static_cast<int>(problemData.binLabels.size()) - positiveLabels;
        if (negativeLabels > positiveLabels) {
            problemData.labelsWeights[0] = 1.0;
            problemData.labelsWeights[1] = 1.0 + log(static_cast<double>(negativeLabels) / positiveLabels);
        } else {
            problemData.labelsWeights[0] = 1.0 + log(static_cast<double>(positiveLabels) / negativeLabels);
            problemData.labelsWeights[1] = 1.0;
        }
    }

    if (args.optimizerType == liblinear) trainLiblinear(problemData, args);
    else trainOnline(problemData, args);

    // Apply threshold and calculate number of non-zero weights
    pruneWeights(args.weightsThreshold);
    if(W->sparseMem() < W->denseMem()){
        auto newW = new SparseVector<Weight>(*W);
        delete W;
        W = newW;
    }

    delete[] problemData.labels;
    delete[] problemData.labelsWeights;
}

void Base::setupOnlineTraining(Args& args, int n, bool startWithDenseW) {
    lossType = args.lossType;

    if (n != 0 && startWithDenseW) {
        W = new Vector<Weight>(n);
        if (args.optimizerType == adagrad) G = new Vector<Weight>(n);
    } else {
        W = new MapVector<Weight>(n);
        if (args.optimizerType == adagrad) G = new MapVector<Weight>(n);
    }

    classCount = 2;
    firstClass = 1;
    t = 0;
}

void Base::finalizeOnlineTraining(Args& args) {
    // Because aux bases needs previous weights, TODO: Change this later
    /*
    if (firstClassCount == t || firstClassCount == 0) {
        classCount = 1;
        if (firstClassCount == 0) firstClass = 1 - firstClass;
    }
    */
    pruneWeights(args.weightsThreshold);
}

double Base::predictValue(Feature* features) {
    if (classCount < 2) return static_cast<double>((1 - 2 * firstClass) * -10);
    double val = W->dot(features);
    if (firstClass == 0) val *= -1;

    return val;
}

double Base::predictProbability(Feature* features) {
    double val = predictValue(features);
    if (lossType == squaredHinge)
        //val = 1.0 / (1.0 + std::exp(-2 * val)); // Probability for squared Hinge loss solver
        val = std::exp(-std::pow(std::max(0.0, 1.0 - val), 2));
    else
        val = 1.0 / (1.0 + std::exp(-val)); // Probability
    return val;
}

void Base::clear() {
    classCount = 0;
    firstClass = 0;
    firstClassCount = 0;
    t = 0;
    delete W;
    W = nullptr;
    delete G;
    G = nullptr;
}

void Base::pruneWeights(double threshold) {
    Weight bias = W->at(1); // Do not prune bias feature
    W->prune(threshold);
    W->insertD(1, bias);
}

void Base::save(std::ostream& out, bool saveGrads) {
    saveVar(out, classCount);
    saveVar(out, firstClass);
    saveVar(out, lossType);

    if (classCount > 1) {
        // Save main weights vector size to estimate optimal representation
        size_t s = W->size();
        size_t n0 = W->nonZero();
        saveVar(out, s);
        saveVar(out, n0);

        W->save(out);
        bool grads = (saveGrads && G != nullptr);
        saveVar(out, grads);
        if(grads) G->save(out);
    }
}

void Base::load(std::istream& in, bool loadGrads, RepresentationType loadAs) {
    loadVar(in, classCount);
    loadVar(in, firstClass);
    loadVar(in, lossType);

    if (classCount > 1) {
        size_t s;
        size_t n0;
        loadVar(in, s);
        loadVar(in, n0);

        // Decide on optimal representation in case of map
        size_t denseSize = Vector<Weight>::estimateMem(s, n0);
        size_t mapSize = MapVector<Weight>::estimateMem(s, n0);
        bool loadSparse = (mapSize < denseSize || s == 0);

        if(loadSparse && loadAs == map){
            W = new MapVector<Weight>();
            G = new MapVector<Weight>();
        }
        else if(loadAs == sparse){
            W = new SparseVector<Weight>();
            G = new SparseVector<Weight>();
        }
        else{
            W = new Vector<Weight>();
            G = new Vector<Weight>();
        }
        W->load(in);

        bool grads;
        loadVar(in, grads);
        if(grads) {
            if(loadGrads) G->load(in);
            else{
                G->skipLoad(in);
                delete G;
            }
        }

//        Log(CERR) << "  Load base: classCount: " << classCount << ", firstClass: "
//                  << firstClass << ", weights: " << nonZeroW << "/" << wSize << "\n";
    }
}

void Base::setFirstClass(int first){
    if(firstClass != first){
        W->invert();
        if(G != nullptr) G->invert();
        firstClass = first;
    }
}

Base* Base::copy() {
    Base* copy = new Base();
    if (W) copy->W = W->copy();
    if (G) copy->G = G->copy();

    copy->firstClass = firstClass;
    copy->classCount = classCount;
    copy->lossType = lossType;

    return copy;
}

Base* Base::copyInverted() {
    Base* c = copy();
    c->W->invert();
    if(c->G != nullptr) c->G->invert();
    return c;
}

void Base::to(RepresentationType type) {
    vecTo(W, type);
    vecTo(G, type);
}

unsigned long long Base::mem(){
    unsigned long long totalMem = sizeof(Base);
    if(W != nullptr) totalMem += W->mem();
    if(G != nullptr) totalMem += G->mem();
    return totalMem;
}

void Base::vecTo(AbstractVector<Weight>* vec, RepresentationType type){
    if(vec == nullptr) return;

    AbstractVector<Weight>* newVec = nullptr;
    if(type == dense && dynamic_cast<Vector<Weight>*>(vec) == nullptr) newVec = new Vector<Weight>(*vec);
    else if(type == map && dynamic_cast<MapVector<Weight>*>(vec) == nullptr) newVec = new MapVector<Weight>(*vec);
    else if(type == sparse && dynamic_cast<SparseVector<Weight>*>(vec) == nullptr) newVec = new SparseVector<Weight>(*vec);
    else throw std::invalid_argument("Unknown representation type");

    if(newVec != nullptr){
        delete vec;
        vec = newVec;
    }
}
