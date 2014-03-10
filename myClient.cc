#include <iostream>

#include "RamCloud.h"
#include "OptionParser.h"
using namespace RAMCloud;

int main(){
	RamCloud client("fast+udp:host=0.0.0.0,port=12246");
	client.createTable("test");
	uint64_t table;
	table=client.getTableId("test");
	std::cout<<table<<std::endl;
	return 0;
}
