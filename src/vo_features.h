/*

The MIT License

Copyright (c) 2015 Avi Singh

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

*/

//ZU9       
#include "opencv2/video/tracking.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/features2d/features2d.hpp"
#include "opencv2/xfeatures2d/nonfree.hpp"
#include "opencv2/calib3d/calib3d.hpp"


#include <iostream>
#include <ctype.h>
#include <algorithm> // for copy
#include <iterator> // for ostream_iterator
#include <vector>
#include <ctime>
#include <sstream>
#include <fstream>
#include <string>
#include <typeinfo>
#include <chrono>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "top_k.h"

/* header file for DNNDK APIs */
#include <dnndk/dnndk.h>

#define INPUT_NODE "ConvNdBackward1"
#define OUTPUT_NODE_semi "ConvNdBackward22"
#define OUTPUT_NODE_desc "ConvNdBackward25"
#define Width 640
#define Height 480
#define Cell 8
#define Feature_Length 65
#define NMS_Threshold 4
#define D 256
#define KEEP_K_POINTS 200
#define NN_thresh 360
#define MATCHER "BF"
#define CONF_thresh 0.15

#undef readl
#define readl(addr) \
    ({ unsigned int __v = (*(volatile unsigned int *) (addr)); __v; })

#undef writel
#define writel(addr,b) (void)((*(volatile unsigned int *) (addr)) = (b))

#define REG_BASE_ADDRESS     0x80000000
#define DDR1_BASE_ADDRESS     0x60000000
#define DDR2_BASE_ADDRESS     0x70000000
#define INST_BASE_ADDRESS     0x6D000000

int     memfd;
void    *mapped_reg_base;
void    *mapped_ddr1_base;
void    *mapped_ddr2_base;
void    *mapped_inst_base;

using namespace cv;
using namespace std;


void *memory_map(unsigned int map_size, off_t base_addr) //map_size = n MByte
{
    void *mapped_base;
    mapped_base = mmap(0, map_size*1024*1024, PROT_READ | PROT_WRITE, MAP_SHARED
, memfd, base_addr);
    if (mapped_base == (void *) -1) {
        printf("Can't map memory to user space.\n");
        exit(0);
    }
#ifdef DEBUG
    printf("Memory mapped at address %p.\n", mapped_base);
#endif
    return mapped_base;
}

void device_setup(DPUKernel *&kernel, DPUTask *&task)
{
    // Attach to DPU driver and prepare for running
    dpuOpen();

    // Load DPU Kernel for DenseBox neural network
    kernel = dpuLoadKernel("superpoint");

	task = dpuCreateTask(kernel, 0);
    
    off_t   reg_base = REG_BASE_ADDRESS;
    off_t   ddr1_base = DDR1_BASE_ADDRESS;
    off_t   ddr2_base = DDR2_BASE_ADDRESS;
    off_t   inst_base = INST_BASE_ADDRESS;  
    
    printf("open\n");
    memfd = open("/dev/mem", O_RDWR | O_SYNC);
    printf("reg\n");
    mapped_reg_base = memory_map(1,reg_base);
    printf("ddr1\n");
    mapped_ddr1_base = memory_map(1024,ddr1_base);
    printf("ddr2\n");
    mapped_ddr2_base = memory_map(1024,ddr2_base);
    printf("inst\n");
    mapped_inst_base = memory_map(1024,inst_base);
    printf("finish\n");
    
    writel(mapped_reg_base+0x1000+0x00,0x00000004);
    writel(mapped_reg_base+0x1000+0x00,0x00000000);
}

void device_close(DPUKernel *kernel, DPUTask *task)
{
    dpuDestroyTask(task);

    // Destroy DPU Kernel & free resources
    dpuDestroyKernel(kernel);

    // Dettach from DPU driver & release resources
    dpuClose();
}

void run_superpoint(DPUTask *task, Mat img, vector<Point2f>& points, Mat& desc)
{
    std::chrono::steady_clock::time_point t1,t2;
    std::chrono::duration<double> time_used;
    
    t1=std::chrono::steady_clock::now();//程序段开始前取得系统运行时间(ms)
    assert(task);
    points.resize(0,Point2f(0,0));
    
    int num = dpuGetInputTensorSize(task, INPUT_NODE);
    int8_t* input_img = new int8_t[num]();
    uint8_t* data = (uint8_t*)img.data;
    // int input_max = 0;
    // int input_min = 128;
    for(int i=0; i<num; i++) {
        input_img[i] = (int8_t)(data[i]/2);
    }
    
    dpuSetInputTensorInHWCInt8(task, INPUT_NODE, (int8_t*)input_img, num);
    
    // cout << "\nRun superpoint ..." << endl;
    dpuRunTask(task);
    
    /* Get DPU execution time (in us) of CONV Task */
    // long long timeProf = dpuGetTaskProfile(task);
    //cout << "  DPU CONV Execution time: " << (timeProf * 1.0f) << "us\n";
    
    int num_semi = dpuGetOutputTensorSize(task, OUTPUT_NODE_semi);
    int8_t* result_semi_int;
    int num_desc = dpuGetOutputTensorSize(task, OUTPUT_NODE_desc);
    int8_t* result_desc_int;
    
    DPUTensor* semi_tensor = dpuGetOutputTensorInHWCInt8(task, OUTPUT_NODE_semi);
    result_semi_int = dpuGetTensorAddress(semi_tensor);
    DPUTensor* desc_tensor = dpuGetOutputTensorInHWCInt8(task, OUTPUT_NODE_desc);
    result_desc_int = dpuGetTensorAddress(desc_tensor);
    
    t2=std::chrono::steady_clock::now();//程序段结束后取得系统运行时间(ms)
    time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    cout << "       DPU time:" << (time_used.count() * 1000) << " ms." << endl;
    
    t1=std::chrono::steady_clock::now();//程序段开始前取得系统运行时间(ms)
    //------------------------softmax----------------------------------
    memcpy(mapped_ddr1_base,result_semi_int,num_semi);
    writel(mapped_reg_base,0x00000000);
    printf("reset\n");
    writel(mapped_reg_base,0x000000aa);
    printf("written gpio\n");
    unsigned int a,b,c;
    int wait_num=0;
    do
    {
        usleep(200);
        a = readl(mapped_reg_base);
        cout<<"wait"<<endl;
        wait_num++;
    }while(!(a&0x00000001) && wait_num<100);
    if(wait_num>=10)return;
    
    //read result
    ofstream outfile;
    //outfile.open ("softmax_out.txt");
    uint16_t* result_semi = (uint16_t*)mapped_ddr2_base;
    point coarse_semi[Height/Cell][Width/Cell];
    for(int i=0; i<Height/Cell; i++) {
        for(int j=0; j<Width/Cell; j++) {
            int channel_id = result_semi[(i*Width/Cell+j)*2+1];
            coarse_semi[i][j].H = i*Cell + channel_id/Cell;
            coarse_semi[i][j].W = j*Cell + channel_id%Cell;
            coarse_semi[i][j].semi = result_semi[(i*Width/Cell+j)*2];
            coarse_semi[i][j].num = i*Width/Cell+j;
            //outfile << channel_id <<" "<< coarse_semi[i][j].semi/4096.0 <<" "<< 4096.0/coarse_semi[i][j].semi <<endl;
            if(coarse_semi[i][j].num>4800 || coarse_semi[i][j].num<0)
            {
                cout<<"coarse_semi[i][j].num:"<< coarse_semi[i][j].num<<endl;
                return;
            }
        }
    }
    //outfile.close();
    t2=std::chrono::steady_clock::now();//程序段结束后取得系统运行时间(ms)
    time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    cout << "       softmax time:" << (time_used.count() * 1000) << " ms." << endl;
    
    //---------------------------------NMS---------------------------------
    t1=std::chrono::steady_clock::now();//程序段开始前取得系统运行时间(ms)
    vector<point> tmp_point;
    for(int i=0; i<Height/Cell; i++) {
        for(int j=0; j<Width/Cell; j++) {
            if(coarse_semi[i][j].semi != 65535) {
                float tmp_semi = coarse_semi[i][j].semi;
                for(int kh=max(0,i-1); kh<min(Height/Cell,i+1+1); kh++)
                    for(int kw=max(0,j-1); kw<min(Width/Cell,j+1+1); kw++)
                        if(i!=kh||j!=kw) {
                            if(abs(coarse_semi[i][j].H-coarse_semi[kh][kw].H)<=NMS_Threshold && abs(coarse_semi[i][j].W-coarse_semi[kh][kw].W)<=NMS_Threshold) {
                                if(tmp_semi<=coarse_semi[kh][kw].semi)
                                    coarse_semi[kh][kw].semi = 65535;
                                else
                                    coarse_semi[i][j].semi = 65535;
                            }
                        }
                if(coarse_semi[i][j].semi!=0)
                    tmp_point.push_back(coarse_semi[i][j]);
            }
        }
    }
    cout<<"tmp_point.size:"<<tmp_point.size()<<endl;
    t2=std::chrono::steady_clock::now();//程序段结束后取得系统运行时间(ms)
    time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    cout << "       MNS time:" << (time_used.count() * 1000) << " ms." << endl;
    
    //rank
    t1=std::chrono::steady_clock::now();//程序段开始前取得系统运行时间(ms)
    
    if(tmp_point.size()>KEEP_K_POINTS)
    {
        bottom_k(tmp_point,tmp_point.size(),KEEP_K_POINTS);
    }
    cout<<"tmp_point.size:"<<tmp_point.size()<<endl;
    
    t2=std::chrono::steady_clock::now();//程序段结束后取得系统运行时间(ms)
    time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    cout << "       rank time:" << (time_used.count() * 1000) << " ms." << endl;
    
    //-------------------------------normalize----------------------
    t1=std::chrono::steady_clock::now();//程序段开始前取得系统运行时间(ms)
    memcpy(mapped_ddr1_base,result_desc_int,num_desc);
    cout<<"unnorm data copy"<<endl;
    //sg write
    unsigned int* sg = (unsigned int*)mapped_inst_base;
    //read
    for(int i=0; i<tmp_point.size()*2; i++) {
        for(int j=0; j<16; j++)
            sg[i*16+j] = 0;
        sg[i*16] = INST_BASE_ADDRESS+(i+1)*64;
        sg[i*16+2] = DDR1_BASE_ADDRESS + tmp_point[i/2].num*256;
        sg[i*16+6] = 0x0C000100;
        if(sg[i*16+2]>DDR1_BASE_ADDRESS + 4800*256)
        {
            cout<<"tmp_point[i/2].num:"<< tmp_point[i/2].num<<endl;
            return;
        }
    }
    //write
    for(int i=tmp_point.size()*2; i<tmp_point.size()*3; i++) {
        for(int j=0; j<16; j++)
            sg[i*16+j] = 0;
        sg[i*16] = INST_BASE_ADDRESS+(i+1)*64;
        sg[i*16+2] = DDR2_BASE_ADDRESS + (i-tmp_point.size()*2)*256;
        sg[i*16+6] = 0x0C000100;
    }
    t2=std::chrono::steady_clock::now();//程序段结束后取得系统运行时间(ms)
    time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    cout << "       write sg time:" << (time_used.count() * 1000) << " ms." << endl;
    
    a = readl(mapped_ddr1_base);
    printf("ddr1[0]:%x result_desc_int[0]:%x\n", a,result_desc_int[0]);
    a = readl(mapped_inst_base+0x00080000);
    printf("sg[0]:%x\n", a);
    a = readl(mapped_ddr2_base);
    printf("ddr2[0]:%x\n", a);
    a = readl(mapped_reg_base+0x1000+0x04);
    printf("MM2S DMA Status register:%x\n", a);
    a = readl(mapped_reg_base+0x1000+0x34);
    printf("S2MM DMA Status register:%x\n", a);
    
    //start norm
    t1=std::chrono::steady_clock::now();//程序段开始前取得系统运行时间(ms)
    cout<<"start norm"<<endl;
    writel(mapped_reg_base+0x1000+0x30,0x00027004);
    printf("write reg\n");
    writel(mapped_reg_base+0x1000+0x30,0x00027000);
    printf("rst\n");
    writel(mapped_reg_base+0x1000+0x38,INST_BASE_ADDRESS+tmp_point.size()*2*16*4);//
    printf("1\n");
    writel(mapped_reg_base+0x1000+0x30,0x00027001);
    printf("2\n");
    writel(mapped_reg_base+0x1000+0x40,INST_BASE_ADDRESS+(tmp_point.size()*3-1)*16*4);//
    printf("3\n");
    
    writel(mapped_reg_base+0x1000+0x00,0x00027000);
    printf("rst\n");
    writel(mapped_reg_base+0x1000+0x08,INST_BASE_ADDRESS+0x00000000);//
    printf("4\n");
    writel(mapped_reg_base+0x1000+0x00,0x00027001);
    printf("5\n"); 
    writel(mapped_reg_base+0x1000+0x10,INST_BASE_ADDRESS+(tmp_point.size()*2-1)*16*4);//
    printf("write reg finish \n");
    
    wait_num=0;
    do
    {
        // a = readl(mapped_ddr2_base+0x1000);
        // printf("ddr2[0]:%x\n", a);
        // a = readl(mapped_reg_base+0x1000+0x04);
        // printf("MM2S DMA Status register:%x\n", a);
        // a = readl(mapped_reg_base+0x1000+0x08);
        // printf("MM2S Current Descriptor Pointer:%x\n", a);
        // a = readl(mapped_reg_base+0x1000+0x10);
        // printf("MM2S Tail Descriptor Pointer:%x\n", a);
        // a = readl(mapped_reg_base+0x1000+0x34);
        // printf("S2MM DMA Status register:%x\n", a);
        // a = readl(mapped_reg_base+0x1000+0x38);
        // printf("S2MM Current Descriptor Pointer:%x\n", a);
        // a = readl(mapped_reg_base+0x1000+0x40);
        // printf("S2MM Tail Descriptor Pointer:%x\n", a);
        usleep(50);
        a = readl(mapped_reg_base+0x1000+0x34);
        cout<<"wait"<<endl;
        wait_num++;
    }while(!(a&0x00000002) && wait_num<100);
    if(wait_num>=10)
    {
        a = readl(mapped_reg_base+0x1000+0x04);
        printf("MM2S DMA Status register:%x\n", a);
        a = readl(mapped_reg_base+0x1000+0x34);
        printf("S2MM DMA Status register:%x\n", a);
        a = readl(mapped_reg_base+0x1000+0x08);
        printf("MM2S DMA CURDESC:%x\n", a);
        b = readl(mapped_inst_base+a-INST_BASE_ADDRESS+0);
        printf("NXTDESC:%x\n", b);
        b = readl(mapped_inst_base+a-INST_BASE_ADDRESS+4);
        printf("NXTDESC_MSB:%x\n", b);
        b = readl(mapped_inst_base+a-INST_BASE_ADDRESS+8);
        printf("BUFFER_ADDRESS:%x\n", b);
        a = readl(mapped_reg_base+0x1000+0x38);
        printf("S2MM DMA CURDESC:%x\n", a);
        
        return;
    }
    t2=std::chrono::steady_clock::now();//程序段结束后取得系统运行时间(ms)
    time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    cout << "       normalize run time:" << (time_used.count() * 1000) << " ms." << endl;
    
    //output desc
    t1=std::chrono::steady_clock::now();//程序段开始前取得系统运行时间(ms)
    int8_t* result_desc = (int8_t*)mapped_ddr2_base;
    
    desc.create( int(tmp_point.size()), D, CV_32FC1); 
    
    for(int i=0;i<tmp_point.size();i++)
    {
        points.push_back(Point2f(tmp_point[i].W, tmp_point[i].H));
        float* pData = desc.ptr<float>(i);   //第i+1行的所有元素  
        for(int j = 0; j < desc.cols; j++)
            pData[j] = result_desc[j+i*D];
    }
        
    t2=std::chrono::steady_clock::now();//程序段结束后取得系统运行时间(ms)
    time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    cout << "       output time:" << (time_used.count() * 1000) << " ms." << endl;
    
    delete[] input_img;
}


Point2d pixel2cam ( const Point2d& p, const Mat& K )
{
    return Point2d
           (
               ( p.x - K.at<double> ( 0,2 ) ) / K.at<double> ( 0,0 ),
               ( p.y - K.at<double> ( 1,2 ) ) / K.at<double> ( 1,1 )
           );
}



void featureTracking_superpoint(DPUTask *task, vector<Point2f>& points1, Mat& desc1, Mat& img_2, Mat depth_1, Mat K, vector<Point3f>& points_3d, vector<Point2f>& points_2d)	{ 

//this function automatically gets rid of points for which tracking fails
    std::chrono::steady_clock::time_point t1,t2;
    std::chrono::duration<double> time_used;
    
    vector<Point2f>points2;
    Mat desc2;
    points_3d.resize(0,Point3f(0,0,0));
    points_2d.resize(0,Point2f(0,0));
    
    t1=std::chrono::steady_clock::now();//程序段开始前取得系统运行时间(ms)
    run_superpoint(task, img_2, points2, desc2);
    t2=std::chrono::steady_clock::now();//程序段结束后取得系统运行时间(ms)
    time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    cout << "   run_superpoint time:" << (time_used.count() * 1000) << " ms." << endl;
    
    t1=std::chrono::steady_clock::now();//程序段开始前取得系统运行时间(ms)
    vector<DMatch> matches;
    if( MATCHER == "BF" ) {
        BFMatcher matcher(NORM_L2, true);
        matcher.match(desc1, desc2, matches);
    }
    else {
        FlannBasedMatcher matcher;
        matcher.match(desc1, desc2, matches);
    }
    cout <<  "desc1 size:" << desc1.size() << endl;
    cout <<  "matches size:" << matches.size() << endl;
    // cout <<  "matches[0].distance:" << matches[0].distance << endl;
    t2=std::chrono::steady_clock::now();//程序段结束后取得系统运行时间(ms)
    time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    cout << "   match time:" << (time_used.count() * 1000) << " ms." << endl;//0.231208s
    
    t1=std::chrono::steady_clock::now();//程序段开始前取得系统运行时间(ms)
    
    //-- 第四步:匹配点对筛选
    double min_dist=10000, max_dist=0;
    //找出所有匹配之间的最小距离和最大距离, 即是最相似的和最不相似的两组点之间的距离
    for ( int i = 0; i < desc1.rows; i++ )
    {
        double dist = matches[i].distance;
        if ( dist < min_dist ) min_dist = dist;
        if ( dist > max_dist ) max_dist = dist;
    }
    printf ( "-- Max dist : %f \n", max_dist );
    printf ( "-- Min dist : %f \n", min_dist );
    
    vector <Point2f> RAN_KP1, RAN_KP2;
    for(int i=0; i<matches.size(); i++) {
        if ( matches[i].distance>NN_thresh )//360)//
            continue;
        if ( abs(points1[matches[i].queryIdx].x-points2[matches[i].trainIdx].x)> Width/10 )
            continue;
        if ( abs(points1[matches[i].queryIdx].y-points2[matches[i].trainIdx].y)> Height/10 )
            continue;
        RAN_KP1.push_back(points1[matches[i].queryIdx]);
        RAN_KP2.push_back(points2[matches[i].trainIdx]);
        //cout << matches[i].distance << endl;
        //RAN_KP1是要存储img01中能与img02匹配的点
    }
    t2=std::chrono::steady_clock::now();//程序段结束后取得系统运行时间(ms)
    time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    cout << "   thresh check time:" << (time_used.count() * 1000) << " ms." << endl;//5.3e-05s
    
    t1=std::chrono::steady_clock::now();//程序段开始前取得系统运行时间(ms)
    cout <<  "RAN_KP1 size:" << RAN_KP1.size() << endl;
    vector<uchar> RansacStatus;
	Mat Fundamental = findFundamentalMat(RAN_KP1, RAN_KP2, RansacStatus, FM_RANSAC);
	//通过RansacStatus来删除误匹配点
    t2=std::chrono::steady_clock::now();//程序段结束后取得系统运行时间(ms)
    time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    cout << "   RANSAC time:" << (time_used.count() * 1000) << " ms." << endl;//0.001139s
    
    t1=std::chrono::steady_clock::now();//程序段开始前取得系统运行时间(ms)
    for ( int i=0; i<RAN_KP1.size(); i++ )
    {
        if (RansacStatus[i] == 0)
            continue;
        ushort d = depth_1.ptr<unsigned short> (int ( RAN_KP1[i].y )) [ int ( RAN_KP1[i].x ) ];
        if ( d == 0 )   // bad depth
            continue;
        float dd = d/5000.0;
        Point2d p1 = pixel2cam ( RAN_KP1[i], K );
        points_3d.push_back ( Point3f ( p1.x*dd, p1.y*dd, dd ) );
        points_2d.push_back ( RAN_KP2[i] );
        
        line(img_2, RAN_KP1[i], RAN_KP2[i], Scalar(0, 0, 255));
        circle(img_2, RAN_KP2[i], 4, cv::Scalar(0, 0, 255));
    }

    cout<<"3d-2d pairs: "<<points_3d.size() <<endl;
        
    //assert( points_3d.size() >= 50 );
    imshow( "Road facing camera", img_2 );
    
    points1 = points2;
    desc1 = desc2;
    
    t2=std::chrono::steady_clock::now();//程序段结束后取得系统运行时间(ms)
    time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    cout << "   2d_to_3d time:" << (time_used.count() * 1000) << " ms." << endl;//0.000659s
}


//局部极大值抑制，这里利用fast特征点的响应值做比较
void selectMax(int r, std::vector<KeyPoint> & kp){

    //r是局部极大值抑制的窗口半径
    if (r != 0){
        //对kp中的点进行局部极大值筛选
        for (int i = 0; i < kp.size(); i++){
            for (int j = i + 1; j < kp.size(); j++){
                //如果两个点的距离小于半径r，则删除其中响应值较小的点
                if (abs(kp[i].pt.x - kp[j].pt.x)<=r && abs(kp[i].pt.y - kp[j].pt.y)<=r){
                    if (kp[i].response < kp[j].response){
                        std::vector<KeyPoint>::iterator it = kp.begin() + i;
                        kp.erase(it);
                        i--;
                        break;
                    }
                    else{
                        std::vector<KeyPoint>::iterator it = kp.begin() + j;
                        kp.erase(it);
                        j--;
                    }
                }
            }
        }
    }

}

void run_orb ( const Mat& img, vector<Point2f>& point, Mat& descriptors )
{
    // used in OpenCV3
    Ptr<FeatureDetector> detector = ORB::create( 2000 );
    Ptr<DescriptorExtractor> descriptor = ORB::create();
    // use this if you are in OpenCV2
    // Ptr<FeatureDetector> detector = FeatureDetector::create ( "ORB" );
    // Ptr<DescriptorExtractor> descriptor = DescriptorExtractor::create ( "ORB" );
    
    //-- 第一步:检测 Oriented FAST 角点位置
    vector<KeyPoint> keypoint;
    detector->detect ( img,keypoint );
    //NMS
    selectMax(NMS_Threshold, keypoint);
    cout << "keypoints size:" << keypoint.size() << endl;
    
    for(int i=0; i<min( KEEP_K_POINTS, int(keypoint.size()-1) ); i++) {
        for(int j=keypoint.size()-1; j>i; j--) {
            if(keypoint[j].response>keypoint[j-1].response) {
                swap(keypoint[j], keypoint[j-1]);
            }
        }
    }
    if(keypoint.size()>KEEP_K_POINTS) keypoint.resize(KEEP_K_POINTS, KeyPoint());

    //-- 第二步:根据角点位置计算 BRIEF 描述子
    descriptor->compute ( img, keypoint, descriptors );
    
    for(int i=0; i<keypoint.size(); i++) {
        point.push_back(keypoint[i].pt);
    }
}

void featureTracking_ORB(vector<Point2f>& keypoint1, Mat& desc1, Mat img_2, Mat depth_1, Mat K, vector<Point3f>& points_3d, vector<Point2f>& points_2d)	

{ 
    points_3d.resize(0,Point3f(0,0,0));
    points_2d.resize(0,Point2f(0,0));
    vector<Point2f>keypoint2;
    Mat desc2;
    
    run_orb(img_2, keypoint2, desc2);
    
    //-- 第三步:对两幅图像中的BRIEF描述子进行匹配，使用 Hamming 距离
    vector<DMatch> matches;
    // BFMatcher matcher ( NORM_HAMMING );
    if( MATCHER == "BF" ) {
        Ptr<DescriptorMatcher> matcher  = DescriptorMatcher::create ( "BruteForce-Hamming" );
        matcher->match ( desc1, desc2, matches );
    }
    else {
         // the descriptor for FlannBasedMatcher should has matrix element of CV_32F
        if( desc1.type()!=CV_32F ) 
            desc1.convertTo( desc1, CV_32F );
        if( desc2.type()!=CV_32F ) 
            desc2.convertTo( desc2, CV_32F );
        Ptr<DescriptorMatcher> matcher  = DescriptorMatcher::create ( "FlannBased" );
        matcher->match ( desc1, desc2, matches );
    }

    //-- 第四步:匹配点对筛选
    double min_dist=10000, max_dist=0;

    //找出所有匹配之间的最小距离和最大距离, 即是最相似的和最不相似的两组点之间的距离
    for ( int i = 0; i < desc1.rows; i++ )
    {
        double dist = matches[i].distance;
        if ( dist < min_dist ) min_dist = dist;
        if ( dist > max_dist ) max_dist = dist;
    }

    printf ( "-- Max dist : %f \n", max_dist );
    printf ( "-- Min dist : %f \n", min_dist );

    //当描述子之间的距离大于两倍的最小距离时,即认为匹配有误.但有时候最小距离会非常小,设置一个经验值30作为下限.
    vector <Point2f> RAN_KP1, RAN_KP2;
    for(int i=0; i<matches.size(); i++) {
        if ( matches[i].distance> max ( 2*min_dist, 300.0 ) )
            continue;
        if ( abs(keypoint1[matches[i].queryIdx].x-keypoint2[matches[i].trainIdx].x)> Width/10 )
            continue;
        if ( abs(keypoint1[matches[i].queryIdx].y-keypoint2[matches[i].trainIdx].y)> Height/10 )
            continue;
        RAN_KP1.push_back(keypoint1[matches[i].queryIdx]);
        RAN_KP2.push_back(keypoint2[matches[i].trainIdx]);
        //RAN_KP1是要存储img01中能与img02匹配的点
    }
    
    //cout <<  "RAN_KP1 size:" << RAN_KP1.size() << endl;
    vector<uchar> RansacStatus;
	Mat Fundamental = findFundamentalMat(RAN_KP1, RAN_KP2, RansacStatus, FM_RANSAC);
	//重新定义关键点RR_KP和RR_matches来存储新的关键点和基础矩阵，通过RansacStatus来删除误匹配点
    
    cout << "RAN_KP1 size:" << RAN_KP1.size() << endl;
    for ( int i=0; i<RAN_KP1.size(); i++ )
    {
        if (RansacStatus[i] == 0)
            continue;
        ushort d = depth_1.ptr<unsigned short> (int ( RAN_KP1[i].y )) [ int ( RAN_KP1[i].x ) ];
        if ( d == 0 )   // bad depth
            continue;
        float dd = d/5000.0;
        Point2d p1 = pixel2cam ( RAN_KP1[i], K );
        points_3d.push_back ( Point3f ( p1.x*dd, p1.y*dd, dd ) );
        points_2d.push_back ( RAN_KP2[i] );
        
        line(img_2, RAN_KP1[i], RAN_KP2[i], Scalar(0, 0, 255));
        circle(img_2, RAN_KP2[i], 4, cv::Scalar(0, 0, 255));
    }

    cout<<"3d-2d pairs: "<<points_3d.size() <<endl;
    imshow( "Road facing camera", img_2 );
    
    keypoint1 = keypoint2;
    //cout << "keypoints size:" << keypoint1.size() << endl;
    desc1 = desc2;
}


void run_sift ( const Mat& img, vector<Point2f>& point, Mat& descriptors )
{
    // used in OpenCV3
    Ptr<FeatureDetector> detector = xfeatures2d::SIFT::create( KEEP_K_POINTS,  3, 0.04, 3, 1.6 );
    Ptr<DescriptorExtractor> descriptor = xfeatures2d::SIFT::create();
    // use this if you are in OpenCV2
    // Ptr<FeatureDetector> detector = FeatureDetector::create ( "ORB" );
    // Ptr<DescriptorExtractor> descriptor = DescriptorExtractor::create ( "ORB" );
    
    //-- 第一步:检测 keypoint 角点位置
    vector<KeyPoint> keypoint;
    detector->detect ( img,keypoint );
    //NMS
    // selectMax(NMS_Threshold, keypoint);
    //cout << "keypoints size:" << keypoint.size() << endl;

    //-- 第二步:根据角点位置计算 BRIEF 描述子
    descriptor->compute ( img, keypoint, descriptors );
    
    for(int i=0; i<keypoint.size(); i++) {
        point.push_back(keypoint[i].pt);
    }
}

void featureTracking_sift(vector<Point2f>& keypoint1, Mat& desc1, Mat img_2, Mat depth_1, Mat K, vector<Point3f>& points_3d, vector<Point2f>& points_2d)	{ 

//this function automatically gets rid of points for which tracking fails
    std::chrono::steady_clock::time_point t1,t2;
    std::chrono::duration<double> time_used;
    
    vector<Point2f>keypoint2;
    Mat desc2;
    points_3d.resize(0,Point3f(0,0,0));
    points_2d.resize(0,Point2f(0,0));
    
    t1=std::chrono::steady_clock::now();//程序段开始前取得系统运行时间(ms)
    run_sift(img_2, keypoint2, desc2);
    t2=std::chrono::steady_clock::now();//程序段结束后取得系统运行时间(ms)
    time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    cout << "   run_sift time:" << (time_used.count() * 1000) << " ms." << endl;
    
    t1=std::chrono::steady_clock::now();//程序段开始前取得系统运行时间(ms)
    vector<DMatch> matches;
    if( MATCHER == "BF" ) {
        BFMatcher matcher(NORM_L2, true);
        matcher.match(desc1, desc2, matches);
    }
    else {
        FlannBasedMatcher matcher;
        matcher.match(desc1, desc2, matches);
    }
    //cout <<  "desc1 size:" << desc1.size() << endl;
    //cout <<  "matches size:" << matches.size() << endl;
    t2=std::chrono::steady_clock::now();//程序段结束后取得系统运行时间(ms)
    time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    cout << "   BFmatch time:" << (time_used.count() * 1000) << " ms." << endl;//0.231208s
    
    //cout << "RAN_KP" << endl;
    vector <Point2f> RAN_KP1, RAN_KP2;
    for(int i=0; i<matches.size(); i++) {
        if ( matches[i].distance> 200 )
            continue;
        if ( abs(keypoint1[matches[i].queryIdx].x-keypoint2[matches[i].trainIdx].x)> Width/10 )
            continue;
        if ( abs(keypoint1[matches[i].queryIdx].y-keypoint2[matches[i].trainIdx].y)> Height/10 )
            continue;
        RAN_KP1.push_back(keypoint1[matches[i].queryIdx]);
        RAN_KP2.push_back(keypoint2[matches[i].trainIdx]);
        //RAN_KP1是要存储img01中能与img02匹配的点
    }
    
    t1=std::chrono::steady_clock::now();//程序段开始前取得系统运行时间(ms)
    //cout <<  "RAN_KP1 size:" << RAN_KP1.size() << endl;
    vector<uchar> RansacStatus;
	Mat Fundamental = findFundamentalMat(RAN_KP1, RAN_KP2, RansacStatus, FM_RANSAC);
	//通过RansacStatus来删除误匹配点
    t2=std::chrono::steady_clock::now();//程序段结束后取得系统运行时间(ms)
    time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    cout << "   RANSAC time:" << (time_used.count() * 1000) << " ms." << endl;//0.001139s
    
    t1=std::chrono::steady_clock::now();//程序段开始前取得系统运行时间(ms)
    for ( int i=0; i<RAN_KP1.size(); i++ )
    {
        if (RansacStatus[i] == 0)
            continue;
        ushort d = depth_1.ptr<unsigned short> (int ( RAN_KP1[i].y )) [ int ( RAN_KP1[i].x ) ];
        if ( d == 0 )   // bad depth
            continue;
        float dd = d/5000.0;
        Point2d p1 = pixel2cam ( RAN_KP1[i], K );
        points_3d.push_back ( Point3f ( p1.x*dd, p1.y*dd, dd ) );
        points_2d.push_back ( RAN_KP2[i] );
        
        line(img_2, RAN_KP1[i], RAN_KP2[i], Scalar(0, 0, 255));
        circle(img_2, RAN_KP2[i], 4, cv::Scalar(0, 0, 255));
    }

    // cout<<"3d-2d pairs: "<<points_3d.size() <<endl;
    imshow( "Road facing camera", img_2 );
    
    keypoint1 = keypoint2;
    desc1 = desc2;
    
    t2=std::chrono::steady_clock::now();//程序段结束后取得系统运行时间(ms)
    time_used = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1);
    cout << "match time:" << (time_used.count() * 1000) << " ms." << endl;
}