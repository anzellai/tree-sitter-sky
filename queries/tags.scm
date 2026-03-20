; tags.scm — Symbol tagging for navigation (go-to-definition, etc.)

(function_declaration
  (lower_identifier) @name) @definition.function

(type_declaration
  (upper_identifier) @name) @definition.type

(type_alias_declaration
  (upper_identifier) @name) @definition.type

(type_annotation_declaration
  (lower_identifier) @name) @reference.function

(type_variant
  (upper_identifier) @name) @definition.constructor

(module_declaration
  (module_name) @name) @definition.module

(import_declaration
  (module_name) @name) @reference.module
