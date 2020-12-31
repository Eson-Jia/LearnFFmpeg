//
// Created by ubuntu on 2020/12/30.
//
#include <fstream>
#include <iostream>
#include <cstring>

using namespace std;
#define BUFF_SIZE 1024

int main(int argc, char **argv) {
    char buff[BUFF_SIZE] = {0};
    sprintf(buff, "%s-%d", "123", 123);

    cout << "buff length:" << strlen(buff) << endl;

    ofstream o1(argv[1]);
    o1.write(buff, 1024);
    o1.close();

    ofstream o2(argv[2]);
    o2 << buff;
    o2.close();

    ofstream o3(argv[3]);
    o3.write(buff, strlen(buff));
    o3.close();

    memset(buff, 0, BUFF_SIZE);
    *buff = 'a';
    ofstream o4(argv[4]);
    o4.write(buff, BUFF_SIZE);
    o4.close();
}