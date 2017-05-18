#include <string>
#include <algorithm>
#include <stdexcept>
#include <boost/shared_ptr.hpp>
#include <boost/optional.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <gtest/gtest.h>
#include <qi/messaging/net/resolve.hpp>
#include <qi/messaging/net/connect.hpp>
#include <qi/messaging/net/networkasio.hpp>
#include <qi/future.hpp>
#include "networkmock.hpp"
#include "networkcommon.hpp"

static const qi::MilliSeconds defaultTimeout{500};

template<typename T>
struct NetworkType
{
  using type = typename T::Network;
};

template<typename T>
using Network = typename NetworkType<T>::type;

namespace qi { namespace net {

template<typename N>
struct ResolveUrlListFun
{
  using Network = N;
  ErrorCode<N> operator()(IoService<N>& io, const Url& url) const
  {
    Promise<ErrorCode<N>> promise;
    ResolveUrlList<N> resolve{io};
    resolve(url,
      [=](ErrorCode<N> err, Iterator<Resolver<N>>) mutable {
        promise.setValue(err);
      }
    );
    return promise.future().value();
  }
};

template<typename N>
struct ResolveUrlFun
{
  using Network = N;
  ErrorCode<N> operator()(IoService<N>& io, const Url& url) const
  {
    Promise<ErrorCode<N>> promise;
    ResolveUrl<N> resolve{io};
    resolve(url, IpV6Enabled{false},
      [=](ErrorCode<N> err, boost::optional<Entry<Resolver<N>>>) mutable {
        promise.setValue(err);
      }
    );
    return promise.future().value();
  }
};

template<typename N>
struct ConnectSocketFun
{
  using Network = N;
  ErrorCode<N> operator()(IoService<N>& io, const Url& url) const
  {
    Promise<ErrorCode<N>> promise;
    ConnectSocket<N> connect{io};
    SslContext<N> context{Method<SslContext<N>>::sslv23};
    connect(url, SslEnabled{true}, context, IpV6Enabled{false},
      HandshakeSide<SslSocket<N>>::client,
      [=](ErrorCode<N> err, boost::shared_ptr<SslSocket<N>>) mutable {
        promise.setValue(err);
      }
    );
    return promise.future().value();
  }
};

template<typename N>
struct ConnectSocketFutureFun
{
  using Network = N;
  ErrorCode<N> operator()(IoService<N>& io, const Url& url) const
  {
    ConnectSocketFuture<N> connect{io};
    SslContext<N> context{Method<SslContext<N>>::sslv23};
    connect(url, SslEnabled{true}, context, IpV6Enabled{false}, HandshakeSide<SslSocket<N>>::client);
    return stringToError(connect.complete().error());
  }
  ErrorCode<N> stringToError(const std::string& s) const
  {
    const auto e = badAddress<ErrorCode<N>>();
    if (code(s) == e.value()) return e;
    throw std::runtime_error("stringToError: unknown error, detail=" + s);
  }
};

}} // qi::net


template<typename T>
struct NetResolveUrl : testing::Test
{
};

using sequences = testing::Types<
  // Mock
  qi::net::ResolveUrlListFun<mock::N>, qi::net::ResolveUrlFun<mock::N>,
  qi::net::ConnectSocketFun<mock::N>, qi::net::ConnectSocketFutureFun<mock::N>,
  // Asio
  qi::net::ResolveUrlListFun<qi::net::NetworkAsio>, qi::net::ResolveUrlFun<qi::net::NetworkAsio>,
  qi::net::ConnectSocketFun<qi::net::NetworkAsio>, qi::net::ConnectSocketFutureFun<qi::net::NetworkAsio>
>;

TYPED_TEST_CASE(NetResolveUrl, sequences);

TYPED_TEST(NetResolveUrl, WrongUrl)
{
  using namespace qi;
  using namespace qi::net;
  using F = TypeParam;
  using N = Network<F>;
  IoService<N>& io = N::defaultIoService();
  {
    const auto error = F{}(io, Url{});
    ASSERT_EQ(badAddress<ErrorCode<N>>(), error);
  }
  {
    const auto error = F{}(io, Url{"abcd"});
    ASSERT_EQ(badAddress<ErrorCode<N>>(), error);
  }
  {
    const auto error = F{}(io, Url{"10.12.14.15.16"});
    ASSERT_EQ(badAddress<ErrorCode<N>>(), error);
  }
  {
    const auto error = F{}(io, Url{"tcp://10.12.14.15"});
    ASSERT_EQ(badAddress<ErrorCode<N>>(), error);
  }
  {
    const auto error = F{}(io, Url{"tcp://10.12.14.15:0"});
    ASSERT_EQ(badAddress<ErrorCode<N>>(), error);
  }
}

TEST(NetFindFirstValidIfAny, Ok)
{
  using namespace qi;
  using namespace qi::net;
  using net::detail::findFirstValidIfAny;
  using mock::N;
  using Entry = N::_resolver_entry;
  auto entry = [](bool v6, std::string host) {
    return Entry{{{v6, std::move(host)}}};
  };
  auto v4_0 = entry(false, "10.11.12.13");
  auto v4_1 = entry(false, "10.11.12.14");
  auto v6_0 = entry(true, "10.11.12.15");
  using I = N::resolver_type::iterator;
  {
    Entry* a[] = {nullptr};
    auto optionalEntry = findFirstValidIfAny(I{a}, I{}, IpV6Enabled{false});
    ASSERT_FALSE(optionalEntry);
  }
  {
    Entry* a[] = {&v4_0, &v4_1, &v6_0, nullptr};
    auto optionalEntry = findFirstValidIfAny(I{a}, I{}, IpV6Enabled{false});
    ASSERT_EQ(v4_0, optionalEntry.value());
    optionalEntry = findFirstValidIfAny(I{a}, I{}, IpV6Enabled{true});
    ASSERT_EQ(v4_0, optionalEntry);
  }
  {
    Entry* a[] = {&v6_0, &v4_0, &v4_1, nullptr};
    auto optionalEntry = findFirstValidIfAny(I{a}, I{}, IpV6Enabled{false});
    ASSERT_EQ(v4_0, optionalEntry.value());
    optionalEntry = findFirstValidIfAny(I{a}, I{}, IpV6Enabled{true});
    ASSERT_EQ(v6_0, optionalEntry.value());
  }
  {
    Entry* a[] = {&v6_0, nullptr};
    auto optionalEntry = findFirstValidIfAny(I{a}, I{}, IpV6Enabled{true});
    ASSERT_EQ(v6_0, optionalEntry.value());
    optionalEntry = findFirstValidIfAny(I{a}, I{}, IpV6Enabled{false});
    ASSERT_FALSE(optionalEntry);
  }
}

TEST(NetResolveUrlList, Success)
{
  using namespace qi;
  using namespace qi::net;
  using namespace mock;
  using mock::Resolver;
  Resolver::async_resolve = defaultAsyncResolve;
  using I = N::resolver_type::iterator;
  Promise<std::pair<Error, I>> promiseResult;
  IoService<N> io;
  const std::string host = "10.11.12.13";
  ResolveUrlList<N> resolve{io};
  resolve(Url{"tcp://" + host + ":1234"},
    [&](Error e, I it) mutable {
      promiseResult.setValue({e, it});
    }
  );
  auto fut = promiseResult.future();
  ASSERT_EQ(FutureState_FinishedWithValue, fut.waitFor(defaultTimeout));
  ASSERT_EQ(success<Error>(), fut.value().first);
  auto it = fut.value().second;
  const N::_resolver_entry entryIpV4{{{false, host}}};
  const N::_resolver_entry entryIpV6{{{true, host}}};
  ASSERT_EQ(entryIpV4, *it);
  ++it;
  ASSERT_EQ(entryIpV6, *it);
}