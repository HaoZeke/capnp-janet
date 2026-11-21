// Reference RPC peer: capnp-C++ serving Adder over rpc-twoparty on
// 127.0.0.1:<argv[1]>. The C vat (rpc_client.c) bootstraps it and
// calls add(), proving protocol-level compatibility with the reference
// implementation, not just wire-format byte equality.
#include "adder.capnp.h"
#include <capnp/rpc-twoparty.h>
#include <capnp/rpc.h>
#include <kj/async-io.h>
#include <kj/async.h>
#include <stdio.h>
#include <stdlib.h>
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

	kj::Promise<void> self(SelfContext ctx) override {
		ctx.getResults().setAdder(thisCap());
		return kj::READY_NOW;
	}

	kj::Promise<void> callBack(CallBackContext ctx) override {
		auto req = ctx.getParams().getCb().addRequest();
		req.setA(20);
		req.setB(22);
		return req.send().then([ctx](auto resp) mutable {
			ctx.getResults().setSum(resp.getSum());
		});
	}

	kj::Promise<void> hang(HangContext) override { return kj::NEVER_DONE; }

	kj::Promise<void> relaySelf(RelaySelfContext ctx) override {
		auto req = ctx.getParams().getTarget().selfRequest();
		return ctx.tailCall(kj::mv(req));
	}

	kj::Promise<void> callSelf(CallSelfContext ctx) override {
		auto req = ctx.getParams().getCb().selfRequest();
		return req.send().then([ctx](auto self) mutable {
			auto add = self.getAdder().addRequest();
			add.setA(1);
			add.setB(2);
			return add.send().then([ctx](auto resp) mutable {
				ctx.getResults().setSum(resp.getSum());
			});
		});
	}

	kj::Promise<void> relay(RelayContext ctx) override {
		auto p = ctx.getParams();
		auto req = p.getTarget().relayRequest();
		req.setA(p.getA());
		req.setB(p.getB());
		return ctx.tailCall(kj::mv(req));
	}
};

// A bootstrap made when the Bootstrap message arrives. It resolves on the
// next turn of the event loop, after capnp-C++ has written the Return, so
// the Return always carries a senderPromise however late the client is.
class PromiseBootstrap final
    : public capnp::BootstrapFactory<capnp::rpc::twoparty::VatId> {
public:
	capnp::Capability::Client
	createFor(capnp::rpc::twoparty::VatId::Reader) override {
		return Adder::Client(kj::evalLater(
		    []() -> Adder::Client { return kj::heap<AdderImpl>(); }));
	}
};

struct Peer {
	Peer(kj::Own<kj::AsyncIoStream> &&s, const std::string &mode)
	    : stream(kj::mv(s)),
	      network(*stream, capnp::rpc::twoparty::Side::SERVER),
	      rpc(mode == "promise"
	              ? kj::heap<capnp::RpcSystem<capnp::rpc::twoparty::VatId>>(
	                    network, factory)
	          : mode == "none"
	              ? kj::heap<capnp::RpcSystem<capnp::rpc::twoparty::VatId>>(
	                    network, kj::Maybe<capnp::Capability::Client>(nullptr))
	              : kj::heap<capnp::RpcSystem<capnp::rpc::twoparty::VatId>>(
	                    network, kj::Maybe<capnp::Capability::Client>(
	                                 Adder::Client(kj::heap<AdderImpl>())))) {}
	kj::Own<kj::AsyncIoStream> stream;
	capnp::TwoPartyVatNetwork network;
	PromiseBootstrap factory;
	kj::Own<capnp::RpcSystem<capnp::rpc::twoparty::VatId>> rpc;
};

static kj::Promise<void> serve(kj::ConnectionReceiver &listener,
                               kj::TaskSet &tasks, const std::string &mode) {
	return listener.accept().then(
	    [&listener, &tasks, mode](kj::Own<kj::AsyncIoStream> &&s) {
		    auto peer = kj::heap<Peer>(kj::mv(s), mode);
		    auto done = peer->network.onDisconnect();
		    tasks.add(done.attach(kj::mv(peer)));
		    return serve(listener, tasks, mode);
	    });
}

struct Ignore final : public kj::TaskSet::ErrorHandler {
	void taskFailed(kj::Exception &&) override {}
};

int main(int argc, char **argv) {
	unsigned int port = argc > 1 ? (unsigned int)atoi(argv[1]) : 0;
	auto io = kj::setupAsyncIo();
	// "promise" serves a bootstrap that capnp-C++ sends as senderPromise
	// and resolves right after; "none" serves no bootstrap at all.
	std::string mode = argc > 2 ? argv[2] : "";
	Ignore ignore;
	kj::TaskSet tasks(ignore);
	auto addr = io.provider->getNetwork()
	                .parseAddress("127.0.0.1", port)
	                .wait(io.waitScope);
	auto listener = addr->listen();
	printf("%u\n", listener->getPort());
	fflush(stdout);
	serve(*listener, tasks, mode).wait(io.waitScope);
}
