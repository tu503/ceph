// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "common/ceph_context.h"
#include "tracer.h"
#include "common/debug.h"

#ifdef HAVE_JAEGER
#include "opentelemetry/sdk/trace/batch_span_processor.h"
#include "opentelemetry/sdk/trace/tracer_provider.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_options.h"

#define dout_subsys ceph_subsys_trace
#undef dout_prefix
#define dout_prefix (*_dout << "otel_tracing: ")

namespace tracing {

const opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> Tracer::noop_tracer = opentelemetry::trace::Provider::GetTracerProvider()->GetTracer("no-op", OPENTELEMETRY_SDK_VERSION);
const jspan_ptr Tracer::noop_span = noop_tracer->StartSpan("noop");

using bufferlist = ceph::buffer::list;

void Tracer::init(CephContext* _cct, opentelemetry::nostd::string_view service_name,
                  opentelemetry::nostd::string_view instance_id,
                  const std::map<std::string, std::string>& extra_attrs) {
  ceph_assert(_cct);
  cct = _cct;
  if (!tracer) {
    ldout(cct, 3) << "tracer was not loaded, initializing tracing" << dendl;
    opentelemetry::exporter::otlp::OtlpHttpExporterOptions exporter_options;
    exporter_options.url = cct->_conf.get_val<std::string>("otel_tracing_endpoint");
    if (exporter_options.url.find("://") == std::string::npos) {
      exporter_options.url = "http://" + exporter_options.url;
    }
    if (exporter_options.url.find("/v1/traces") == std::string::npos) {
      exporter_options.url += "/v1/traces";
    }
    ldout(cct, 3) << "OTLP HTTP exporter endpoint: " << exporter_options.url << dendl;
    auto otlp_exporter = opentelemetry::exporter::otlp::OtlpHttpExporterFactory::Create(exporter_options);
    opentelemetry::sdk::trace::BatchSpanProcessorOptions processor_options;
    processor_options.max_queue_size = 16384;
    processor_options.max_export_batch_size = 4096;
    opentelemetry::sdk::resource::ResourceAttributes res_attrs{{"service.name", service_name}};
    if (!instance_id.empty()) {
      res_attrs.SetAttribute("service.instance.id", instance_id);
    }
    for (const auto& [k, v] : extra_attrs) {
      res_attrs.SetAttribute(opentelemetry::nostd::string_view(k),
                             opentelemetry::common::AttributeValue(opentelemetry::nostd::string_view(v)));
    }
    const auto resource = opentelemetry::sdk::resource::Resource::Create(std::move(res_attrs));
    auto processor = std::unique_ptr<opentelemetry::sdk::trace::SpanProcessor>(new opentelemetry::sdk::trace::BatchSpanProcessor(std::move(otlp_exporter), processor_options));
    const auto provider = opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider>(new opentelemetry::sdk::trace::TracerProvider(std::move(processor), resource));
    opentelemetry::trace::Provider::SetTracerProvider(provider);
    tracer = provider->GetTracer(service_name, OPENTELEMETRY_SDK_VERSION);
  }
}

void Tracer::init_from_global(CephContext* _cct, opentelemetry::nostd::string_view scope_name) {
  ceph_assert(_cct);
  cct = _cct;
  if (!tracer) {
    ldout(cct, 3) << "binding tracer scope '" << scope_name << "' to global provider" << dendl;
    auto provider = opentelemetry::trace::Provider::GetTracerProvider();
    tracer = provider->GetTracer(scope_name, OPENTELEMETRY_SDK_VERSION);
  }
}

jspan_ptr Tracer::start_trace(opentelemetry::nostd::string_view trace_name) {
  ceph_assert(cct);
  if (is_enabled()) {
    ceph_assert(tracer);
    ldout(cct, 20) << "start trace for " << trace_name << " " << dendl;
    return tracer->StartSpan(trace_name);
  }
  return noop_span;
}

jspan_ptr Tracer::start_trace(opentelemetry::nostd::string_view trace_name, bool trace_is_enabled) {
  ceph_assert(cct);
  ldout(cct, 20) << "start trace enabled " << trace_is_enabled << " " << dendl;
  if (trace_is_enabled) {
    ceph_assert(tracer);
    ldout(cct, 20) << "start trace for " << trace_name << " " << dendl;
    return tracer->StartSpan(trace_name);
  }
  return noop_tracer->StartSpan(trace_name);
}

jspan_ptr Tracer::add_span(opentelemetry::nostd::string_view span_name, const jspan_ptr& parent_span) {
  if (is_enabled() && parent_span && parent_span->IsRecording()) {
    opentelemetry::trace::StartSpanOptions span_opts;
    span_opts.parent = parent_span->GetContext();
    ldout(cct, 20) << "adding span " << span_name << " " << dendl;
    return tracer->StartSpan(span_name, span_opts);
  }
  return noop_span;
}

jspan_ptr Tracer::add_span(opentelemetry::nostd::string_view span_name, const jspan_context& parent_ctx) {
  if (parent_ctx.IsValid()) {
    ceph_assert(tracer);
    opentelemetry::trace::StartSpanOptions span_opts;
    span_opts.parent = parent_ctx;
    ldout(cct, 20) << "adding span " << span_name << " " << dendl;
    return tracer->StartSpan(span_name, span_opts);
  }
  return noop_span;
}

bool Tracer::is_enabled() const {
  return cct && cct->_conf->jaeger_tracing_enable;
}

} // namespace tracing

#endif // HAVE_JAEGER

