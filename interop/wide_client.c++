// Calls Wide.make and checks a multi-field result.
#include "wide.capnp.h"
#include <capnp/ez-rpc.h>
#include <kj/async.h>
#include <cstdio>
#include <string>

int main(int argc, char **argv) {
	std::string port = argc > 1 ? argv[1] : "43117";
	capnp::EzRpcClient client("127.0.0.1:" + port);
	auto &wait = client.getWaitScope();
	auto wide = client.getMain<Wide>();
	auto req = wide.makeRequest();
	req.setN(20);
	auto resp = req.send().wait(wait);
	if (resp.getLo() != 19 || resp.getHi() != 21 || !resp.getFlag() ||
	    resp.getName() != "wide") {
		fprintf(stderr, "wide result mismatch\n");
		return 1;
	}
	printf("ok wide result\n");
	return 0;
}
