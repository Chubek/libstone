"""Validate and expand the bounded ArchDef instruction-template subset."""

import re

from .diagnostics import ArchonError


_PLACEHOLDER = re.compile(r"\{([A-Za-z_][A-Za-z0-9_]*)\}")
_MAX_DEFINITIONS = 4096
_MAX_TEMPLATE_TEXT = 16384


def _fail(node, message):
    raise ArchonError(message, node["loc"])


def _width_parameter(operand, parameters):
    spec = operand["type"]
    if isinstance(spec, dict) and spec["kind"] != "named" and spec["arg"] in parameters:
        return spec["arg"]
    return None


def _check_template(template, max_width):
    """Check parameter types and uses independently of any instantiations."""
    parameters = {}
    for param in template["parameters"]:
        spec = param["spec"]
        if spec["kind"] == "bits" and not 1 <= spec["width"] <= max_width:
            _fail(param, f"template parameter {param['name']!r} bit width must be "
                         f"between 1 and {max_width}")
        parameters[param["name"]] = spec

    body = template["body"]
    for operand in body["operands"]:
        name = operand["name"]
        if name in parameters:
            _fail(operand, f"operand {name!r} conflicts with a template parameter")
        width = _width_parameter(operand, parameters)
        if width is not None and parameters[width]["kind"] != "nat":
            _fail(operand, f"template parameter {width!r} must have type 'nat' to set an immediate width")

    for binding in body["encoding"].values():
        if isinstance(binding, dict):
            name = binding["parameter"]
            if parameters[name]["kind"] not in {"bits", "nat"}:
                _fail(binding, f"template parameter {name!r} must have type 'bits<N>' or 'nat' "
                               "to set an encoding constant")

    text = body["assembly"]
    if len(text) > _MAX_TEMPLATE_TEXT:
        _fail(body, f"template assembly exceeds {_MAX_TEMPLATE_TEXT} characters")
    remaining = _PLACEHOLDER.sub("", text)
    if "{" in remaining or "}" in remaining:
        _fail(body, "invalid assembly placeholder in template body")
    for match in _PLACEHOLDER.finditer(text):
        name = match[1]
        if name in parameters and parameters[name]["kind"] != "string":
            _fail(body, f"template parameter {name!r} must have type 'string' for assembly interpolation")
    return parameters


def _bind(definition, template, parameters, max_width):
    arguments = definition["arguments"]
    if len(arguments) != len(parameters):
        raise ArchonError(
            f"template {template['name']!r} expects {len(parameters)} argument(s), "
            f"got {len(arguments)} (declared at {template['loc']})",
            definition["template_loc"],
        )
    bound = dict(zip(parameters, arguments))
    for name, arg in bound.items():
        spec = parameters[name]
        kind, value = spec["kind"], arg["value"]
        context = f"argument for template parameter {name!r} of {template['name']!r}"
        if kind == "string":
            if not isinstance(value, str):
                _fail(arg, f"{context} must be a string literal")
            if len(value) > _MAX_TEMPLATE_TEXT:
                _fail(arg, f"{context} exceeds {_MAX_TEMPLATE_TEXT} characters")
        elif kind == "nat":
            if type(value) is not int or value < 0:
                _fail(arg, f"{context} must be a natural number")
            if value.bit_length() > max_width:
                _fail(arg, f"{context} exceeds the {max_width}-bit integer limit")
        else:
            width = spec["width"]
            if type(value) is not int or value < 0 or value.bit_length() > width:
                _fail(arg, f"{context} must be an integer fitting bits<{width}>")
    return bound


def _interpolate(body, bound):
    """Replace only original template-parameter tokens, never rescan inserted text."""
    text = body["assembly"]
    size = cursor = 0

    def replace(match):
        nonlocal size, cursor
        name = match[1]
        if name not in bound:
            return match[0]
        arg = bound[name]
        value = arg["value"]
        if any(ch in "{}" or (ch != "\t" and not ch.isprintable()) for ch in value):
            _fail(arg, f"assembly argument for {name!r} must be printable and cannot contain braces")
        size += match.start() - cursor + len(value)
        cursor = match.end()
        if size > _MAX_TEMPLATE_TEXT:
            _fail(arg, f"expanded assembly exceeds {_MAX_TEMPLATE_TEXT} characters")
        return value

    result = _PLACEHOLDER.sub(replace, text)
    if len(result) > _MAX_TEMPLATE_TEXT:
        _fail(body, f"expanded assembly exceeds {_MAX_TEMPLATE_TEXT} characters")
    return result


def _expand(definition, template, parameters, max_width):
    bound = _bind(definition, template, parameters, max_width)
    body = template["body"]
    operands = []
    for operand in body["operands"]:
        width = _width_parameter(operand, bound)
        if width is not None:
            spec = {**operand["type"], "arg": bound[width]["value"]}
            operand = {**operand, "type": spec}
        operands.append(operand)
    encoding = {
        field: bound[binding["parameter"]]["value"] if isinstance(binding, dict) else binding
        for field, binding in body["encoding"].items()
    }
    return {
        **body, "name": definition["name"], "operands": operands, "encoding": encoding,
        "assembly": _interpolate(body, bound), "instantiation": definition,
    }


def elaborate_instructions(groups, *, max_width):
    """Yield ordinary and instantiated instructions together in name order."""
    templates, definitions = groups["template"], groups["def"]
    if len(definitions) > _MAX_DEFINITIONS:
        _fail(list(definitions.values())[_MAX_DEFINITIONS],
              f"architecture may have at most {_MAX_DEFINITIONS} template definitions")
    parameters = {name: _check_template(template, max_width)
                  for name, template in templates.items()}
    instructions = groups["instruction"]
    for name in sorted(instructions.keys() | definitions.keys()):
        if name in instructions:
            yield instructions[name]
            continue
        definition = definitions[name]
        reference = definition["template"]
        if reference not in templates:
            raise ArchonError(f"unknown instruction template {reference!r}", definition["template_loc"])
        yield _expand(definition, templates[reference], parameters[reference], max_width)
