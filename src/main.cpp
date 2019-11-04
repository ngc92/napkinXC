/**
 * Copyright (c) 2018 by Marek Wydmuch
 * All rights reserved.
 */

#include <iostream>
#include <iomanip>

#include "args.h"
#include "data_reader.h"
#include "model.h"
#include "types.h"
#include "utils.h"


void train(Args &args) {
    SRMatrix<Label> labels;
    SRMatrix<Feature> features;

    args.printArgs();
    makeDir(args.output);
    args.saveToFile(joinPath(args.output, "args.bin"));

    // Create data reader and load train data
    std::shared_ptr<DataReader> reader = dataReaderFactory(args);
    reader->readData(labels, features, args);
    reader->saveToFile(joinPath(args.output, "data_reader.bin"));

    // Create and train model (train function also saves model)
    std::shared_ptr<Model> model = modelFactory(args);
    model->train(labels, features, args, args.output);
    model->printInfo();

    std::cerr << "All done!\n";
}

void test(Args &args) {
    TimeHelper timer;
    timer.start();

    SRMatrix<Label> labels;
    SRMatrix<Feature> features;

    // Load model args
    args.loadFromFile(joinPath(args.output, "args.bin"));
    args.printArgs();

    // Create data reader and load test data
    std::shared_ptr<DataReader> reader = dataReaderFactory(args);
    reader->loadFromFile(joinPath(args.output, "data_reader.bin"));
    reader->readData(labels, features, args);

    timer.checkpoint();
    timer.printTime();

    // Load model and test
    std::shared_ptr<Model> model = modelFactory(args);
    model->load(args, args.output);

    timer.checkpoint();
    timer.printTime();

    model->test(labels, features, args);
    model->printInfo();

    timer.checkpoint();
    timer.printTime();

    std::cerr << "All done!\n";
}


void predict(Args &args) {
    // Load model args
    args.loadFromFile(joinPath(args.output, "args.bin"));
    args.printArgs();

    // Create data reader
    std::shared_ptr<DataReader> reader = dataReaderFactory(args);
    reader->loadFromFile(joinPath(args.output, "data_reader.bin"));

    // Load model
    std::shared_ptr<Model> model = modelFactory(args);
    model->load(args, args.output);

    std::cout << std::setprecision(5);

    // Predict data from cin and output to cout
    if(args.input == "-"){
        //TODO
    }

    // Read data from file and output prediction to cout
    else {
        SRMatrix<Label> labels;
        SRMatrix<Feature> features;
        reader->readData(labels, features, args);

        std::vector<Prediction> prediction;
        prediction.reserve(model->outputSize());
        //for(int r = 0; r < features.rows(); ++r){
        for(int r = 0; r < 5000; ++r){
            model->predict(prediction, features.row(r), args);

            // Print prediction
            std::cout << labels.row(r)[0];
            for(const auto& p : prediction)
                std::cout << " " << p.label << ":" << p.value;
            std::cout << std::endl;
            prediction.clear();
        }
    }
}

/*
void buildTree(Args &args) {
    args.printArgs();

    SRMatrix<Label> labels;
    SRMatrix<Feature> features;
    args.readData(labels, features);

    PLTree tree;
    tree.buildTreeStructure(labels, features, args);
}

void shrink(Args &args) {
    args.printArgs();

    PLTree tree;
    tree.load(args.input + "/tree.bin");

    std::ifstream in(args.input + "/weights.bin");
    std::ofstream out(args.output + "/weights.bin");
    for (int i = 0; i < tree.nodes(); ++i){
        Base base;
        base.load(in, args);
        base.threshold(args.threshold);
        base.save(out, args);
    }
    in.close();
    out.close();
}
 */

int main(int argc, char** argv) {
    std::vector<std::string> arg(argv, argv + argc);
    Args args = Args();

    // Parse args
    args.parseArgs(arg);

    if(args.command == "train")
        train(args);
    else if(args.command == "test")
        test(args);
    else if(args.command == "predict")
        predict(args);

    return 0;
}
