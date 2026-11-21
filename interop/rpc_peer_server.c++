// Reference RPC peer: capnp-C++ serving Adder over rpc-twoparty on
// 127.0.0.1:<argv[1]>. The C vat (rpc_client.c) bootstraps it and
// calls add(), proving protocol-level compatibility with the reference
// implementation, not just wire-format byte equality.
#include "adder.capnp.h"
#include <capnp/ez-rpc.h>
#include <kj/async.h>
#include <stdio.h>
#include <string.h>
#include <string>

class AdderImpl final : public Adder::Server {
public:
	kj::Promise<void> add(AddContext ctx) override {
		auto p = ctx.getParams();
		ctx.getResults().setSum(p.getA() + p.getB());
		return kj::READY_NOW;
	}

	kj::Promise<void> echo(EchoContext ctx) override {
		auto data = ctx.getResults().initData(ctx.getParams().getN());
		memset(data.begin(), 0xab, data.size());
		return kj::READY_NOW;
	}
};

int main(int argc, char **argv) {
	std::string port = argc > 1 ? argv[1] : "0";
	capnp::EzRpcServer server(kj::heap<AdderImpl>(), "127.0.0.1:" + port);
	auto &waitScope = server.getWaitScope();
	unsigned int bound = server.getPort().wait(waitScope);
	printf("%u\n", bound);
	fflush(stdout);
	kj::NEVER_DONE.wait(waitScope);
}
