#include <iostream>

#include "Cycles.h"
#include "RamCloud.h"
#include "OptionParser.h"
using namespace RAMCloud;

int main(){
	RamCloud client("fast+udp:host=0.0.0.0,port=12246");
	//client.createTable("test");
	uint64_t table;
	uint64_t b;
	table=client.getTableId("test");
	std::cout<<"table id="<<table<<std::endl;
	b=Cycles::rdtsc();
	client.write(table, "43", 2, "hello...", 14);
	std::cout<<"elapsed time="<<Cycles::rdtsc()-b<<std::endl;
	std::cout<<"write successful!"<<std::endl;
	Buffer buffer;
	b=Cycles::rdtsc();
	client.read(table, "42", 2, &buffer);
	std::cout<<"elapsed time="<<Cycles::rdtsc()-b<<std::endl;
	uint32_t length=buffer.getTotalLength();
	std::cout<<"length="<<length<<std::endl;
	std::cout<<static_cast<const char*>(buffer.getRange(0, length))<<std::endl;
	return 0;
}
