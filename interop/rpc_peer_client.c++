// capnp-C++ client for the Adder served by this vat. Connects to
// 127.0.0.1:<port>, bootstraps, and checks add(20, 22) == 42. With
// `abort` the call is expected to fail; the reason is printed. With
// `hold` the response is kept while the peer gets 200 ms more to send.
#include "adder.capnp.h"
#include <capnp/ez-rpc.h>
#include <kj/async.h>
#include <kj/async-io.h>
#include <cstdio>
#include <string>

int main(int argc, char **argv) {
	std::string port = argc > 1 ? argv[1] : "43117";
	std::string mode = argc > 2 ? argv[2] : "";
	capnp::EzRpcClient client("127.0.0.1:" + port);
	auto &wait = client.getWaitScope();
	auto adder = client.getMain<Adder>();
	auto req = adder.addRequest();
	req.setA(20);
	req.setB(22);
	if (mode == "hold") {
		auto resp = req.send().wait(wait);
		client.getIoProvider()
		    .getTimer()
		    .afterDelay(200 * kj::MILLISECONDS)
		    .wait(wait);
		printf("cpp client: held sum %lld\n", (long long)resp.getSum());
		return 0;
	}
	if (mode == "abort") {
		try {
			req.send().wait(wait);
		} catch (const kj::Exception &e) {
			printf("cpp client: %s\n", e.getDescription().cStr());
			// Time for the Abort to reach the peer before exit.
			client.getIoProvider()
			    .getTimer()
			    .afterDelay(200 * kj::MILLISECONDS)
			    .wait(wait);
			return 0;
		}
		fprintf(stderr, "cpp client: call succeeded\n");
		return 1;
	}
	auto resp = req.send().wait(wait);
	if (resp.getSum() != 42) {
		fprintf(stderr, "cpp client: sum %lld\n", (long long)resp.getSum());
		return 1;
	}
	printf("ok cpp client\n");
	return 0;
}
