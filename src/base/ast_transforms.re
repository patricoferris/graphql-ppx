type t = (Schema.schema, Graphql_ast.document) => Graphql_ast.document;

// get's the type of a field name
let unsafe_get_field_type = (schema, ty: Schema.type_meta, name) => {
  let ty_fields =
    switch (ty) {
    | Interface({im_fields, _}) => im_fields
    | Object({om_fields, _}) => om_fields
    | _ => []
    };
  Schema.(
    List.find(fm => fm.Schema.fm_name == name, ty_fields).fm_field_type
    |> Graphql_printer.type_ref_name
    |> Schema.lookup_type(schema)
    |> Option.unsafe_unwrap
  );
};

let traverse_selection_set = (schema, ty, selection_set, fn) => {
  Graphql_ast.(
    selection_set
    |> List.map(
         fun
         | Graphql_ast.Field(
             {item: {fd_selection_set: Some(selection)}} as field,
           ) => {
             let field_ty =
               unsafe_get_field_type(schema, ty, field.item.fd_name.item);
             let selection_set =
               fn(selection.span, schema, field_ty, selection.item);

             Field({
               ...field,
               item: {
                 ...field.item,
                 fd_selection_set: Some({...selection, item: selection_set}),
               },
             });
           }
         | other => other,
       )
  );
};

/**
 * This is a GraphQL AST transform that will add the __typename to all selection
 * sets. This is necessary for Apollo, as this is an implicit field.
 * If we don't include this when using Apollo, the __typename information is
 * lost when we parse values and then serialize back to the js-values again
 */
let rec do_add_typename_to_selection_set =
        (parent_span, schema, ty, selection_set) => {
  let add_typename =
    switch (ty, selection_set) {
    | (_, []) => false
    // do not add typename to a single fragment spread. It will be added in the
    // fragment anyway, and it will prevent the result to be a single record of
    // the type of the fragment
    | (_, [Graphql_ast.FragmentSpread(_)]) => false
    // do not inject `__typename` in interfaces and unions, the printer does that
    // already behind the scenes
    | (Schema.Interface(_), selection_set) => false
    | (Schema.Union(_), selection_set) => false
    | (Schema.Object({om_name}), selection_set) =>
      // do not inject __typename in top-level subscription type
      // not exactly sure why, but this was there in the previous implementation
      if (schema.Schema.meta.sm_subscription_type == Some(om_name)) {
        false;
      } else {
        true;
      }
    | (_, selection_set) => false
    };
  let selection_set =
    traverse_selection_set(
      schema,
      ty,
      selection_set,
      do_add_typename_to_selection_set,
    );

  if (add_typename) {
    [
      Graphql_ast.Field({
        span: parent_span,
        item: {
          fd_alias: None,
          fd_name: {
            span: parent_span,
            item: "__typename",
          },
          fd_arguments: None,
          fd_directives: [],
          fd_selection_set: None,
        },
      }),
      ...selection_set,
    ];
  } else {
    selection_set;
  };
};
/**
 * This is a GraphQL AST transform that removes the `__typename` field from
 * unions. The PPX will add this in the printer stage, so it is always there.
 * The PPX will not allow any scalar fields on a union except for the inline
 * fragments, so we make sure to remove it before doing any further processing.
 */
let rec do_remove_typename_from_union =
        (parent_span, schema, ty, selection_set) => {
  let selection_set =
    switch (ty) {
    | Schema.Interface(_)
    | Schema.Union(_) =>
      Graphql_ast.(
        selection_set
        |> List.fold_left(
             acc =>
               fun
               | Field({item: {fd_name: {item: "__typename"}}}) => acc
               | other => [other, ...acc],
             [],
           )
        |> List.rev
      )
    | _ => selection_set
    };
  traverse_selection_set(
    schema,
    ty,
    selection_set,
    do_remove_typename_from_union,
  );
};

let traverse_document_selections = (fn, schema: Schema.schema, definitions) => {
  Graphql_ast.(
    definitions
    |> List.map(def => {
         switch (def) {
         | Operation({item as op, span}) =>
           let ty_name =
             switch (op.o_type) {
             | Query => schema.meta.sm_query_type
             | Mutation => Option.unsafe_unwrap(schema.meta.sm_mutation_type)
             | Subscription =>
               Option.unsafe_unwrap(schema.meta.sm_subscription_type)
             };
           let ty =
             Schema.lookup_type(schema, ty_name) |> Option.unsafe_unwrap;

           Operation({
             span,
             item: {
               ...op,
               o_selection_set: {
                 item: fn(span, schema, ty, op.o_selection_set.item),
                 span: op.o_selection_set.span,
               },
             },
           });

         | Fragment({item as f, span}) =>
           let ty_name = f.fg_type_condition.item;
           let ty =
             Schema.lookup_type(schema, ty_name) |> Option.unsafe_unwrap;

           Fragment({
             item: {
               ...f,
               fg_selection_set: {
                 item: fn(span, schema, ty, f.fg_selection_set.item),
                 span: f.fg_selection_set.span,
               },
             },
             span,
           });
         }
       })
  );
};

let add_typename_to_selection_set =
  traverse_document_selections(do_add_typename_to_selection_set);
let remove_typename_from_union =
  traverse_document_selections(do_remove_typename_from_union);
