// Dumps the plug-in's parameter descriptors (as described through the OFX
// API) to JSON, so the web editor's parameter schema can be checked against
// the real plug-in. Usage: wmp_ofx_describe <path/to/WronMotionPath.ofx>
#include <cstdio>
#include <string>

#include "../ofx/tests/mock_host.h"
#include "wmp/json.h"

using wmp::json::Value;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s plugin.ofx\n", argv[0]);
        return 2;
    }
    mock::Host host;
    std::string err;
    if (!host.load(argv[1], err)) {
        std::fprintf(stderr, "load failed: %s\n", err.c_str());
        return 1;
    }
    Value root = Value::object();
    root.set("identifier", Value::string(host.plugin()->pluginIdentifier));
    Value params = Value::array();
    for (const auto& p : host.contextDescriptor()->params.params) {
        Value o = Value::object();
        o.set("id", Value::string(p->name));
        o.set("type", Value::string(p->type));
        o.set("label", Value::string(p->props.getString(kOfxPropLabel)));
        o.set("parent", Value::string(p->props.getString(kOfxParamPropParent)));
        auto nums = [&](const char* name) {
            Value a = Value::array();
            const auto it = p->props.props.find(name);
            if (it == p->props.props.end()) return a;
            if (it->second.type == mock::Property::T::Dbl)
                for (double d : it->second.d) a.push(Value::number(d));
            else if (it->second.type == mock::Property::T::Int)
                for (int i : it->second.i) a.push(Value::number(i));
            return a;
        };
        if (p->type != kOfxParamTypeString) o.set("default", nums(kOfxParamPropDefault));
        o.set("min", nums(kOfxParamPropMin));
        o.set("max", nums(kOfxParamPropMax));
        o.set("displayMin", nums(kOfxParamPropDisplayMin));
        o.set("displayMax", nums(kOfxParamPropDisplayMax));
        o.set("animates", Value::boolean(p->props.getInt(kOfxParamPropAnimates) != 0));
        o.set("persistent",
              Value::boolean(!p->props.has(kOfxParamPropPersistant) || p->props.getInt(kOfxParamPropPersistant) != 0));
        o.set("secret", Value::boolean(p->props.getInt(kOfxParamPropSecret) != 0));
        Value opts = Value::array();
        for (int i = 0; i < p->props.dimension(kOfxParamPropChoiceOption); ++i)
            opts.push(Value::string(p->props.getString(kOfxParamPropChoiceOption, i)));
        if (p->type == kOfxParamTypeChoice) o.set("options", opts);
        params.push(o);
    }
    root.set("params", params);
    const std::string s = wmp::json::serialize(root);
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    return 0;
}
