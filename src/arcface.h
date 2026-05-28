#ifndef ARCFACE_H
#define ARCFACE_H

#include <cmath>
#include <vector>
#include <string>
#include "net.h"
#include <opencv2/highgui.hpp>

using namespace std;



float calcSimilar(std::vector<float> feature1, std::vector<float> feature2);


class Arcface {
const string project_path="/home/pi4/LiveFaceReco_RaspberryPi";

public:
    Arcface(string model_folder = ".");
    ~Arcface();
    cv::Mat getFeature(cv::Mat img);

private:
    ncnn::Net net;

    const int feature_dim = 128;

    void normalize(vector<float> &feature);
};

#endif
