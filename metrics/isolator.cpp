#include <map>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/module/isolator.hpp>
#include <mesos/module/module.hpp>

#include <process/address.hpp>
#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/assert.hpp>
#include <stout/hashset.hpp>
#include <stout/ip.hpp>
#include <stout/nothing.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "isolator.hpp"

#include "metrics/messages.pb.h"

namespace inet = process::network::inet;
namespace unix = process::network::unix;

using namespace mesos;
using namespace process;

using std::map;
using std::string;
using std::vector;

using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesosphere {
namespace dcos {
namespace metrics {

class MetricsIsolatorProcess
  : public process::Process<MetricsIsolatorProcess>
{
public:
  MetricsIsolatorProcess(const isolator::Flags& _flags) : flags(_flags)
  {
    // Set `serviceScheme` based on flags.
    ASSERT(flags.service_scheme.isSome());
    ASSERT(flags.service_scheme.get() == "http" ||
           flags.service_scheme.get() == "https");
    serviceScheme = flags.service_scheme.get();

    // Set `service*Address` based on flags.
    ASSERT(flags.service_address.isSome());

    ASSERT(flags.service_network.isSome());
    ASSERT(flags.service_network.get() == "inet" ||
           flags.service_network.get() == "unix");

    if (flags.service_network.get() == "inet") {
      vector<string> hostport =
        strings::split(flags.service_address.get(), ":");
      if (hostport.size() != 2) {
        LOG(FATAL) << "Unable to split '" << flags.service_address.get() << "'"
                   << " into valid 'host:port' combination";
      }

      Try<net::IP> ip = net::IP::parse(hostport[0]);
      if (ip.isError()) {
        LOG(FATAL) << "Unable to parse '" << hostport[0] << "'"
                   << " as a valid IP address: " << ip.error();
      }

      Try<uint16_t> port = numify<uint16_t>(hostport[1]);
      if (port.isError()) {
        LOG(FATAL) << "Unable parse '" + hostport[1] + "'"
                   << " as a valid port of type 'uint16_t': " + port.error();
      }

      serviceInetAddress = inet::Address(ip.get(), port.get());
    }

    if (flags.service_network.get() == "unix") {
      Try<unix::Address> address =
        unix::Address::create(flags.service_address.get());
      if (address.isError()) {
        LOG(FATAL) << "Unable to convert '" + flags.service_address.get() + "'"
                   << " to valid 'unix' address: " + address.error();
      }

      serviceUnixAddress = address.get();
    }

    // Set `serviceEndpoint` based on flags.
    ASSERT(flags.service_endpoint.isSome());
    serviceEndpoint = flags.service_endpoint.get();
  }

  // Let the metrics service know about the container being launched.
  // In the response, grab the STATSD_UDP_HOST and STATSD_UDP_PORT
  // pair being returned and set it in the environment of the
  // `ContainerLaunchInfo` returned from this function.
  virtual Future<Option<ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const slave::ContainerConfig& containerConfig)
  {
    // Let the metrics service know about the container being
    // launched via an HTTP request. In the response, grab the
    // STATSD_UDP_HOST and STATSD_UDP_PORT pair being returned and set
    // it in the environment of the `ContainerLaunchInfo` returned
    // from this function. On any errors, return a `Failure()`.
    ContainerLaunchInfo launchInfo;
    return launchInfo;
  }

  virtual Future<Nothing> cleanup(
      const ContainerID& containerId)
  {
    // Let the metrics service know about the container being
    // destroyed via an HTTP request. On any errors, return an
    // `Failure()`.
    return Nothing();
  }

private:
  const isolator::Flags flags;
  string serviceScheme;
  string serviceEndpoint;
  Option<inet::Address> serviceInetAddress;
  Option<unix::Address> serviceUnixAddress;
};


MetricsIsolator::MetricsIsolator(const isolator::Flags& flags)
  : process(new MetricsIsolatorProcess(flags))
{
  spawn(process.get());
}


MetricsIsolator::~MetricsIsolator()
{
  terminate(process.get());
  wait(process.get());
}


Future<Option<ContainerLaunchInfo>> MetricsIsolator::prepare(
    const ContainerID& containerId,
    const slave::ContainerConfig& containerConfig)
{
  return dispatch(process.get(),
                  &MetricsIsolatorProcess::prepare,
                  containerId,
                  containerConfig);
}


Future<Nothing> MetricsIsolator::cleanup(
    const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &MetricsIsolatorProcess::cleanup,
                  containerId);
}

} // namespace metrics {
} // namespace dcos {
} // namespace mesosphere {

mesos::modules::Module<Isolator>
com_mesosphere_dcos_MetricsIsolatorModule(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Mesosphere",
    "support@mesosphere.com",
    "Metrics Isolator Module.",
    nullptr,
    [](const Parameters& parameters) -> Isolator* {
      // Convert `parameters` into a map.
      map<string, string> values;
      foreach (const Parameter& parameter, parameters.parameter()) {
        values[parameter.key()] = parameter.value();
      }

      mesosphere::dcos::metrics::isolator::Flags flags;

      // Load and validate flags.
      Try<flags::Warnings> load = flags.load(values, false);

      if (load.isError()) {
        LOG(ERROR) << "Failed to parse parameters: " << load.error();
        return nullptr;
      }

      foreach (const flags::Warning& warning, load->warnings) {
        LOG(WARNING) << warning.message;
      }

      return new mesosphere::dcos::metrics::MetricsIsolator(flags);
    });