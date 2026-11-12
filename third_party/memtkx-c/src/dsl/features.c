#include "memtkx/dsl/features.h"

const char *mtkx_feature_name(mtkx_feature feature) {
  switch (feature) {
    case MTKX_FEATURE_PIPELINE:
      return "Pipeline";
    case MTKX_FEATURE_OPERATORS:
      return "Operators";
    case MTKX_FEATURE_PATTERN_MATCH:
      return "PatternMatch";
    case MTKX_FEATURE_AST:
      return "AST";
    case MTKX_FEATURE_REWRITE:
      return "Rewrite";
    case MTKX_FEATURE_EXPR_TEMPLATES:
      return "ExprTemplates";
    case MTKX_FEATURE_CUSTOM_LITERALS:
      return "CustomLiterals";
    case MTKX_FEATURE_MEMOIZATION:
      return "Memoization";
    case MTKX_FEATURE_LAZY:
      return "LazyFeature";
    case MTKX_FEATURE_MONADIC:
      return "Monadic";
    case MTKX_FEATURE_RESULT:
      return "ResultFeature";
    case MTKX_FEATURE_COMBINATOR_PARSER:
      return "CombinatorParser";
    case MTKX_FEATURE_TASK_PIPELINE:
      return "TaskPipeline";
  }
  return "unknown";
}
