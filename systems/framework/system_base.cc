#include "drake/systems/framework/system_base.h"

#include <atomic>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <unordered_set>

#include <fmt/format.h>

#include "drake/common/hash.h"
#include "drake/common/unused.h"
#include "drake/systems/framework/fixed_input_port_value.h"

namespace {

// Output a string like "System::EvalInput()".
std::string FmtFunc(const char* func) {
  return fmt::format("System::{}()", func);
}

}

namespace drake {
namespace systems {

SystemBase::~SystemBase() {}

internal::SystemId SystemBase::get_next_id() {
  return internal::SystemId::get_new_id();
}

std::string SystemBase::GetMemoryObjectName() const {
  // Remove the template parameter(s).
  const std::string type_name_without_templates = std::regex_replace(
      NiceTypeName::Get(*this), std::regex("<.*>$"), std::string());

  // Replace "::" with "/" because ":" is System::GetSystemPathname's separator.
  // TODO(sherm1) Change the separator to "/" and avoid this!
  const std::string default_name = std::regex_replace(
      type_name_without_templates, std::regex(":+"), std::string("/"));

  // Append the address spelled like "@0123456789abcdef".
  const uintptr_t address = reinterpret_cast<uintptr_t>(this);
  return fmt::format("{}@{:0>16x}", default_name, address);
}

std::string SystemBase::GetSystemPathname() const {
  const std::string parent_path =
      get_parent_service() ? get_parent_service()->GetParentPathname()
                           : std::string();
  return parent_path + internal::SystemMessageInterface::path_separator() +
         GetSystemName();
}

CacheEntry& SystemBase::DeclareCacheEntry(
    std::string description, ValueProducer value_producer,
    std::set<DependencyTicket> prerequisites_of_calc) {
  return DeclareCacheEntryWithKnownTicket(
      assign_next_dependency_ticket(), std::move(description),
      std::move(value_producer), std::move(prerequisites_of_calc));
}

CacheEntry& SystemBase::DeclareCacheEntryWithKnownTicket(
    DependencyTicket known_ticket, std::string description,
    ValueProducer value_producer,
    std::set<DependencyTicket> prerequisites_of_calc) {
  // If the prerequisite list is empty the CacheEntry constructor will throw
  // a logic error.
  const CacheIndex index(num_cache_entries());
  cache_entries_.emplace_back(std::make_unique<CacheEntry>(
      this, index, known_ticket, std::move(description),
      std::move(value_producer), std::move(prerequisites_of_calc)));
  CacheEntry& new_entry = *cache_entries_.back();
  return new_entry;
}

void SystemBase::InitializeContextBase(ContextBase* context_ptr) const {
  DRAKE_DEMAND(context_ptr != nullptr);
  ContextBase& context = *context_ptr;

  // Initialization should happen only once per Context.
  DRAKE_DEMAND(
      !internal::SystemBaseContextBaseAttorney::is_context_base_initialized(
          context));

  internal::SystemBaseContextBaseAttorney::set_system_name(
      &context, get_name());
  internal::SystemBaseContextBaseAttorney::set_system_id(
      &context, system_id_);

  // Add the independent-source trackers and wire them up appropriately. That
  // includes input ports since their dependencies are external.
  CreateSourceTrackers(&context);

  DependencyGraph& graph = context.get_mutable_dependency_graph();

  // Create the Context cache containing a CacheEntryValue corresponding to
  // each CacheEntry, add a DependencyTracker and subscribe it to its
  // prerequisites as specified in the CacheEntry. Cache entries are
  // necessarily ordered such that the first cache entry can depend only
  // on the known source trackers created above, the second may depend on
  // those plus the first, and so on. Circular dependencies are not permitted.
  Cache& cache = context.get_mutable_cache();
  for (CacheIndex index(0); index < num_cache_entries(); ++index) {
    const CacheEntry& entry = get_cache_entry(index);
    CacheEntryValue& cache_value = cache.CreateNewCacheEntryValue(
        entry.cache_index(), entry.ticket(), entry.description(),
        entry.prerequisites(), &graph);
    // TODO(sherm1) Supply initial value on creation instead and get rid of
    // this separate call.
    cache_value.SetInitialValue(entry.Allocate());

    if (entry.is_disabled_by_default())
      cache_value.disable_caching();
  }

  // Create the output port trackers yᵢ here. Nothing in this System may
  // depend on them; subscribers will be input ports from peer subsystems or
  // an exported output port in the parent Diagram. The associated cache entries
  // were just created above. Any intra-system prerequisites are set up now.
  for (const auto& oport : output_ports_) {
    internal::SystemBaseContextBaseAttorney::AddOutputPort(
        &context, oport->get_index(), oport->ticket(),
        oport->GetPrerequisite());
  }

  internal::SystemBaseContextBaseAttorney::mark_context_base_initialized(
      &context);
}

// Set up trackers for variable-numbered independent sources: discrete and
// abstract state, numerical and abstract parameters, and input ports.
// The generic trackers like "all parameters" are already present in the
// supplied Context, but we have to subscribe them to the individual
// elements now.
void SystemBase::CreateSourceTrackers(ContextBase* context_ptr) const {
  ContextBase& context = *context_ptr;

  // Define a lambda to do the repeated work below: create trackers for
  // individual entities and subscribe the group tracker to each of them.
  auto make_trackers = [&context](
      DependencyTicket subscriber_ticket,
      const std::vector<TrackerInfo>& system_ticket_info,
      void (*add_ticket_to_context)(ContextBase*, DependencyTicket)) {
    DependencyGraph& graph = context.get_mutable_dependency_graph();
    DependencyTracker& subscriber =
        graph.get_mutable_tracker(subscriber_ticket);

    for (const auto& info : system_ticket_info) {
      auto& source_tracker =
          graph.CreateNewDependencyTracker(info.ticket, info.description);
      add_ticket_to_context(&context, info.ticket);
      subscriber.SubscribeToPrerequisite(&source_tracker);
    }
  };

  // Allocate trackers for each discrete variable group xdᵢ, and subscribe
  // the "all discrete variables" tracker xd to those.
  make_trackers(
      xd_ticket(), discrete_state_tickets_,
      &internal::SystemBaseContextBaseAttorney::AddDiscreteStateTicket);

  // Allocate trackers for each abstract state variable xaᵢ, and subscribe
  // the "all abstract variables" tracker xa to those.
  make_trackers(
      xa_ticket(), abstract_state_tickets_,
      &internal::SystemBaseContextBaseAttorney::AddAbstractStateTicket);

  // Allocate trackers for each numeric parameter pnᵢ and each abstract
  // parameter paᵢ, and subscribe the pn and pa trackers to them.
  make_trackers(
      pn_ticket(), numeric_parameter_tickets_,
      &internal::SystemBaseContextBaseAttorney::AddNumericParameterTicket);
  make_trackers(
      pa_ticket(), abstract_parameter_tickets_,
      &internal::SystemBaseContextBaseAttorney::AddAbstractParameterTicket);

  // Allocate trackers for each input port uᵢ, and subscribe the "all input
  // ports" tracker u to them. Doesn't use TrackerInfo so can't use the lambda.
  for (const auto& iport : input_ports_) {
    internal::SystemBaseContextBaseAttorney::AddInputPort(
        &context, iport->get_index(), iport->ticket(),
        MakeFixInputPortTypeChecker(iport->get_index()));
  }
}

// The only way for a system to evaluate its own input port is if that
// port is fixed. In that case the port's value is in the corresponding
// subcontext and we can just return it. Otherwise, the port obtains its value
// from some other system and we need our parent's help to get access to
// that system.
const AbstractValue* SystemBase::EvalAbstractInputImpl(
    const char* func, const ContextBase& context,
    InputPortIndex port_index) const {
  if (port_index >= num_input_ports())
    ThrowInputPortIndexOutOfRange(func, port_index);

  if (input_ports_[port_index]->get_deprecation().has_value())
    WarnPortDeprecation(/* is_input = */ true, port_index);

  const FixedInputPortValue* const free_port_value =
      context.MaybeGetFixedInputPortValue(port_index);

  if (free_port_value != nullptr)
    return &free_port_value->get_value();  // A fixed input port.

  // The only way to satisfy an input port of a root System is to make it fixed.
  // Since it wasn't fixed, it is unconnected.
  if (get_parent_service() == nullptr) return nullptr;

  // If this is a root Context, our parent can't evaluate it.
  if (context.is_root_context()) return nullptr;

  // This is not the root System, and the port isn't fixed, so ask our parent to
  // evaluate it.
  return get_parent_service()->EvalConnectedSubsystemInputPort(
      *internal::SystemBaseContextBaseAttorney::get_parent_base(context),
      get_input_port_base(port_index));
}

void SystemBase::ThrowNegativePortIndex(const char* func,
                                        int port_index) const {
  DRAKE_DEMAND(port_index < 0);
  throw std::out_of_range(
      fmt::format("{}: negative port index {} is illegal. (System {})",
                  FmtFunc(func), port_index, GetSystemPathname()));
}

void SystemBase::ThrowInputPortIndexOutOfRange(const char* func,
                                               InputPortIndex port) const {
  throw std::out_of_range(fmt::format(
      "{}: there is no input port with index {} because there "
      "are only {} input ports in system {}.",
      FmtFunc(func),  port, num_input_ports(), GetSystemPathname()));
}

void SystemBase::ThrowOutputPortIndexOutOfRange(const char* func,
                                                OutputPortIndex port) const {
  throw std::out_of_range(fmt::format(
      "{}: there is no output port with index {} because there "
      "are only {} output ports in system {}.",
      FmtFunc(func), port,
      num_output_ports(), GetSystemPathname()));
}

void SystemBase::ThrowNotAVectorInputPort(const char* func,
                                          InputPortIndex port) const {
  throw std::logic_error(fmt::format(
      "{}: vector port required, but input port '{}' (index {}) was declared "
          "abstract. Even if the actual value is a vector, use "
          "EvalInputValue<V> instead for an abstract port containing a vector "
          "of type V. (System {})",
      FmtFunc(func),  get_input_port_base(port).get_name(), port,
      GetSystemPathname()));
}

void SystemBase::ThrowInputPortHasWrongType(
    const char* func, InputPortIndex port, const std::string& expected_type,
    const std::string& actual_type) const {
  ThrowInputPortHasWrongType(
      func, GetSystemPathname(), port, get_input_port_base(port).get_name(),
      expected_type, actual_type);
}

void SystemBase::ThrowInputPortHasWrongType(
    const char* func, const std::string& system_pathname, InputPortIndex port,
    const std::string& port_name, const std::string& expected_type,
    const std::string& actual_type) {
  throw std::logic_error(fmt::format(
      "{}: expected value of type {} for input port '{}' (index {}) "
          "but the actual type was {}. (System {})",
      FmtFunc(func), expected_type, port_name, port, actual_type,
      system_pathname));
}

void SystemBase::ThrowCantEvaluateInputPort(const char* func,
                                            InputPortIndex port) const {
  throw std::logic_error(
      fmt::format("{}: input port '{}' (index {}) is neither connected nor "
                      "fixed so cannot be evaluated. (System {})",
                  FmtFunc(func), get_input_port_base(port).get_name(), port,
                  GetSystemPathname()));
}

void SystemBase::ThrowValidateContextMismatch(
    const ContextBase& context) const {
  const char* const info_link =
      "For more information about Context-System mismatches, see "
      "https://drake.mit.edu/"
      "troubleshooting.html#framework-context-system-mismatch";

  // Check if we are a subsystem within a Diagram and the user passed us the
  // root context instead of our subsystem context. In that case, we can provide
  // a more specific error message.
  if (get_parent_service() != nullptr) {
    // N.B. get_parent_service() is only non-null for subsystems in Diagrams.
    const internal::SystemId root_id =
        get_parent_service()->GetRootSystemBase().get_system_id();
    if (context.get_system_id() == root_id) {
      throw std::logic_error(fmt::format(
          "A function call on a {} system named '{}' was passed the root "
          "Diagram's Context instead of the appropriate subsystem Context. "
          "Use GetMyContextFromRoot() or similar to acquire the appropriate "
          "subsystem Context.\n{}",
          this->GetSystemType(), this->GetSystemPathname(), info_link));
    }
  }

  // Check if the context is a sub-context whose root context was created by
  // this Diagram. In that case, we can provide a more specific error message.
  const ContextBase& root_context = [&context]() -> const ContextBase& {
    const ContextBase* iterator = &context;
    while (true)  {
      const ContextBase* parent =
          internal::SystemBaseContextBaseAttorney::get_parent_base(*iterator);
      if (parent == nullptr) {
        return *iterator;
      }
      iterator = parent;
    }
  }();
  if (root_context.get_system_id() == get_system_id()) {
    throw std::logic_error(fmt::format(
        "A function call on the root Diagram was passed a subcontext "
        "associated with its subsystem named '{}' instead of the root "
        "context. When calling a function on a the root Digram, you must "
        "pass a reference to the root Context, not a subcontext.\n{}",
        context.GetSystemPathname(), info_link));
  }

  throw std::logic_error(fmt::format(
      "A function call on a {} system named '{}' was passed the Context of "
      "a system named '{}' instead of the appropriate subsystem Context.\n{}",
      this->GetSystemType(), this->GetSystemPathname(),
      context.GetSystemPathname(), info_link));
}

void SystemBase::ThrowNotCreatedForThisSystemImpl(
    const std::string& nice_type_name, internal::SystemId id) const {
  if (!id.is_valid()) {
    throw std::logic_error(fmt::format(
        "{} was not associated with any System but should have been "
        "created for {} System {}",
        nice_type_name, GetSystemType(), GetSystemPathname()));
  } else {
    throw std::logic_error(fmt::format("{} was not created for {} System {}",
                                       nice_type_name, GetSystemType(),
                                       GetSystemPathname()));
  }
}

void SystemBase::WarnPortDeprecation(bool is_input, int port_index) const {
  // Locate the deprecated PortBase (while sanity-checking our arguments).
  PortBase* port;
  if (is_input) {
    port = input_ports_.at(port_index).get();
  } else {
    port = output_ports_.at(port_index).get();
  }
  DRAKE_DEMAND(port != nullptr);
  DRAKE_DEMAND(port->get_deprecation().has_value());

  // If this port object has already been warned about, then return quickly.
  std::atomic<bool>* const deprecation_already_warned =
      internal::PortBaseAttorney::deprecation_already_warned(port);
  const bool had_already_warned = deprecation_already_warned->exchange(true);
  if (had_already_warned) {
    return;
  }

  // The had_already_warned above is a *per instance* warning, for performance.
  // We'd like to warn at most once *per process*; therefore, we have a second
  // layer of checking, using a unique lookup key for SystemType + PortBase.
  drake::internal::FNV1aHasher hash;
  hash_append(hash, this->GetSystemType());
  hash_append(hash, is_input);
  hash_append(hash, port->get_name());
  const size_t key = size_t{hash};
  static std::mutex g_mutex;
  static std::unordered_set<size_t> g_warned_hash;
  {
    std::lock_guard lock(g_mutex);
    const bool inserted = g_warned_hash.insert(key).second;
    if (!inserted) {
      // The key was already in the map, which means that we've already
      // warned about this port name on this particular system subclass.
      return;
    }
  }

  // We hadn't warned yet, so we'll warn now.
  const std::string& description = port->GetFullDescription();
  const std::string& deprecation = port->get_deprecation().value();
  const char* const message = deprecation.size() ? deprecation.c_str() :
      "no deprecation details were provided";
  drake::log()->warn("{} is deprecated: {}", description, message);
}

std::string SystemBase::GetUnsupportedScalarConversionMessage(
    const std::type_info& source_type,
    const std::type_info& destination_type) const {
  unused(source_type);
  return fmt::format(
      "System {} of type {} does not support scalar conversion to type {}",
      GetSystemPathname(), GetSystemType(),
      NiceTypeName::Get(destination_type));
}

namespace internal {

std::string DiagramSystemBaseAttorney::GetUnsupportedScalarConversionMessage(
    const SystemBase& system, const std::type_info& source_type,
    const std::type_info& destination_type) {
  return system.GetUnsupportedScalarConversionMessage(
      source_type, destination_type);
}

}  // namespace internal

}  // namespace systems
}  // namespace drake
