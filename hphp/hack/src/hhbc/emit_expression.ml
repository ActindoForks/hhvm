(**
 * Copyright (c) 2017, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the "hack" directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 *
*)

open Hh_core
open Hhbc_ast
open Instruction_sequence
open Ast_class_expr
open Ast_scope

module A = Ast
module H = Hhbc_ast
module TC = Hhas_type_constraint
module SN = Naming_special_names
module SU = Hhbc_string_utils
module ULS = Unique_list_string

(* Locals, array elements, and properties all support the same range of l-value
 * operations. *)
module LValOp = struct
  type t =
  | Set
  | SetRef
  | SetOp of eq_op
  | IncDec of incdec_op
  | Unset
end

let is_incdec op =
  match op with
  | LValOp.IncDec _ -> true
  | _ -> false

let is_global_namespace env =
  Namespace_env.is_global_namespace (Emit_env.get_namespace env)

let is_special_function env e args =
  match snd e with
  | A.Id (_, s) ->
  begin
    let n = List.length args in
    match s with
    | "isset" -> n > 0
    | "empty" -> n = 1
    | "tuple" when Emit_env.is_hh_syntax_enabled () -> true
    | "define" when is_global_namespace env ->
      begin match args with
      | [_, A.String _; _] -> true
      | _ -> false
      end
    | "eval" -> n = 1
    | "idx" -> n = 2 || n = 3
    | "class_alias" ->
      begin
        match args with
        | [_, A.String _; _, A.String _]
        | [_, A.String _; _, A.String _; _] -> true
        | _ -> false
      end
   | _ -> false
  end
  | _ -> false

let optimize_null_check () =
  Hhbc_options.optimize_null_check !Hhbc_options.compiler_options

let optimize_cuf () =
  Hhbc_options.optimize_cuf !Hhbc_options.compiler_options

let hack_arr_compat_notices () =
  Hhbc_options.hack_arr_compat_notices !Hhbc_options.compiler_options

(* Emit a comment in lieu of instructions for not-yet-implemented features *)
let emit_nyi description =
  instr (IComment (H.nyi ^ ": " ^ description))

let make_vec_like_array p es = p, A.Array (List.map es ~f:(fun e -> A.AFvalue e))
let make_kvarray p kvs =
  p, A.Array (List.map kvs ~f:(fun (k, v) -> A.AFkvalue (k, v)))

(* Strict binary operations; assumes that operands are already on stack *)
let from_binop op =
  let ints_overflow_to_ints =
    Hhbc_options.ints_overflow_to_ints !Hhbc_options.compiler_options in
  match op with
  | A.Plus -> instr (IOp (if ints_overflow_to_ints then Add else AddO))
  | A.Minus -> instr (IOp (if ints_overflow_to_ints then Sub else  SubO))
  | A.Star -> instr (IOp (if ints_overflow_to_ints then Mul else MulO))
  | A.Slash -> instr (IOp Div)
  | A.Eqeq -> instr (IOp Eq)
  | A.EQeqeq -> instr (IOp Same)
  | A.Starstar -> instr (IOp Pow)
  | A.Diff -> instr (IOp Neq)
  | A.Diff2 -> instr (IOp NSame)
  | A.Lt -> instr (IOp Lt)
  | A.Lte -> instr (IOp Lte)
  | A.Gt -> instr (IOp Gt)
  | A.Gte -> instr (IOp Gte)
  | A.Dot -> instr (IOp Concat)
  | A.Amp -> instr (IOp BitAnd)
  | A.Bar -> instr (IOp BitOr)
  | A.Ltlt -> instr (IOp Shl)
  | A.Gtgt -> instr (IOp Shr)
  | A.Cmp -> instr (IOp Cmp)
  | A.Percent -> instr (IOp Mod)
  | A.Xor -> instr (IOp BitXor)
  | A.LogXor -> instr (IOp Xor)
  | A.Eq _ -> emit_nyi "Eq"
  | A.AMpamp
  | A.BArbar ->
    failwith "short-circuiting operator cannot be generated as a simple binop"

let binop_to_eqop op =
  let ints_overflow_to_ints =
    Hhbc_options.ints_overflow_to_ints !Hhbc_options.compiler_options in
  match op with
  | A.Plus -> Some (if ints_overflow_to_ints then PlusEqual else PlusEqualO)
  | A.Minus -> Some (if ints_overflow_to_ints then MinusEqual else MinusEqualO)
  | A.Star -> Some (if ints_overflow_to_ints then MulEqual else MulEqualO)
  | A.Slash -> Some DivEqual
  | A.Starstar -> Some PowEqual
  | A.Amp -> Some AndEqual
  | A.Bar -> Some OrEqual
  | A.Xor -> Some XorEqual
  | A.Ltlt -> Some SlEqual
  | A.Gtgt -> Some SrEqual
  | A.Percent -> Some ModEqual
  | A.Dot -> Some ConcatEqual
  | _ -> None

let unop_to_incdec_op op =
  let ints_overflow_to_ints =
    Hhbc_options.ints_overflow_to_ints !Hhbc_options.compiler_options in
  match op with
  | A.Uincr -> Some (if ints_overflow_to_ints then PreInc else PreIncO)
  | A.Udecr -> Some (if ints_overflow_to_ints then PreDec else PreDecO)
  | A.Upincr -> Some (if ints_overflow_to_ints then PostInc else PostIncO)
  | A.Updecr -> Some (if ints_overflow_to_ints then PostDec else PostDecO)
  | _ -> None

let collection_type = function
  | "Vector"    -> CollectionType.Vector
  | "Map"       -> CollectionType.Map
  | "Set"       -> CollectionType.Set
  | "Pair"      -> CollectionType.Pair
  | "ImmVector" -> CollectionType.ImmVector
  | "ImmMap"    -> CollectionType.ImmMap
  | "ImmSet"    -> CollectionType.ImmSet
  | x -> failwith ("unknown collection type '" ^ x ^ "'")

let istype_op lower_fq_id =
  match lower_fq_id with
  | "is_int" | "is_integer" | "is_long" -> Some OpInt
  | "is_bool" -> Some OpBool
  | "is_float" | "is_real" | "is_double" -> Some OpDbl
  | "is_string" -> Some OpStr
  | "is_array" -> Some OpArr
  | "is_object" -> Some OpObj
  | "is_null" -> Some OpNull
  | "is_scalar" -> Some OpScalar
  | "hh\\is_keyset" -> Some OpKeyset
  | "hh\\is_dict" -> Some OpDict
  | "hh\\is_vec" -> Some OpVec
  | "hh\\is_varray" -> Some OpVArray
  | "hh\\is_darray" -> Some OpDArray
  | _ -> None

(* See EmitterVisitor::getPassByRefKind in emitter.cpp *)
let get_passByRefKind is_splatted expr  =
  let open PassByRefKind in
  let rec from_non_list_assignment permissive_kind expr =
    match snd expr with
    | A.New _ | A.Lvar _ | A.Clone _
    | A.Import ((A.Include | A.IncludeOnce), _) -> AllowCell
    | A.Binop(A.Eq None, (_, A.List _), e) ->
      from_non_list_assignment WarnOnCell e
    | A.Array_get(_, Some _) -> permissive_kind
    | A.Binop(A.Eq _, _, _) -> WarnOnCell
    | A.Unop((A.Uincr | A.Udecr | A.Usilence), _) -> WarnOnCell
    | A.Call((_, A.Id (_, "eval")), _, [_], []) ->
      WarnOnCell
    | A.Call((_, A.Id (_, "array_key_exists")), _, [_; _], []) ->
      AllowCell
    | A.Call((_, A.Id (_, ("idx"))), _, ([_; _] | [_; _; _]), []) ->
      AllowCell
    | A.Call((_, A.Id (_, ("hphp_array_idx"))), _, [_; _; _], []) ->
      AllowCell
    | A.Xml _ ->
      AllowCell
    | A.NewAnonClass _ -> ErrorOnCell
    | _ -> if is_splatted then AllowCell else ErrorOnCell in
  from_non_list_assignment AllowCell expr

let get_queryMOpMode need_ref op =
  match op with
  | QueryOp.InOut -> MemberOpMode.InOut
  | QueryOp.CGet -> MemberOpMode.Warn
  | QueryOp.Empty when need_ref -> MemberOpMode.Define
  | _ -> MemberOpMode.ModeNone

let is_local_this env id =
  let scope = Emit_env.get_scope env in
  id = SN.SpecialIdents.this
  && Ast_scope.Scope.has_this scope
  && not (Ast_scope.Scope.is_toplevel scope)

let is_legal_lval_op_on_this op =
  match op with
  | LValOp.Unset -> true
  | LValOp.IncDec _ -> true
  | _ -> false

let check_shape_key (pos,name) =
  if String.length name > 0 && String_utils.is_decimal_digit name.[0]
  then Emit_fatal.raise_fatal_parse
    pos "Shape key names may not start with integers"

let extract_shape_field_name_pstring = function
  | A.SFlit s ->
    check_shape_key s; A.String s
  | A.SFclass_const ((pn, _) as id, p) -> A.Class_const ((pn, A.Id id), p)

let rec text_of_expr e_ = match e_ with
  | A.Id id | A.Lvar id | A.String id -> id
  | A.Array_get ((p, A.Lvar (_, id)), Some (_, e_)) ->
    (p, id ^ "[" ^ snd (text_of_expr e_) ^ "]")
  | _ -> Pos.none, "unknown" (* TODO: get text of expression *)

let add_include ?(doc_root=false) e =
  let strip_backslash p =
    let len = String.length p in
    if len > 0 && p.[0] = '/' then String.sub p 1 (len-1) else p in
  let rec split_var_lit = function
    | _, A.Binop (A.Dot, e1, e2) -> begin
      let v, l = split_var_lit e2 in
      if v = ""
      then let var, lit = split_var_lit e1 in var, lit ^ l
      else v, ""
    end
    | _, A.String (_, lit) -> "", lit
    | _, e_ -> snd (text_of_expr e_), "" in
  let var, lit = split_var_lit e in
  let var, lit =
    if var = "__DIR__" then ("", strip_backslash lit) else (var, lit) in
  let inc =
    if var = ""
    then
      if not (Filename.is_relative lit)
      then Hhas_symbol_refs.Absolute lit
      else
        if doc_root
        then Hhas_symbol_refs.DocRootRelative lit
        else Hhas_symbol_refs.SearchPathRelative lit
    else Hhas_symbol_refs.IncludeRootRelative (var, strip_backslash lit) in
  Emit_symbol_refs.add_include inc

let rec expr_and_new env instr_to_add_new instr_to_add = function
  | A.AFvalue e ->
    let add_instr =
      if expr_starts_with_ref e then instr_add_new_elemv else instr_to_add_new
    in
    gather [emit_expr ~need_ref:false env e; add_instr]
  | A.AFkvalue (k, v) ->
    let add_instr =
      if expr_starts_with_ref v then instr_add_elemv else instr_to_add
    in
    gather [
      emit_two_exprs env (fst k) k v;
      add_instr;
    ]

and get_local env (pos, str) =
  if str = SN.SpecialIdents.dollardollar
  then
    match Emit_env.get_pipe_var env with
    | None -> Emit_fatal.raise_fatal_runtime pos
      "Pipe variables must occur only in the RHS of pipe expressions"
    | Some v -> v
  else Local.Named str

and check_non_pipe_local e =
  match e with
  | _, A.Lvar (pos, str) when str = SN.SpecialIdents.dollardollar ->
    Emit_fatal.raise_fatal_parse pos
      "Cannot take indirect reference to a pipe variable"
  | _ -> ()

(*
and get_non_pipe_local (pos, str) =
  if str = SN.SpecialIdents.dollardollar
  then Emit_fatal.raise_fatal_parse pos
    "Cannot take indirect reference to a pipe variable"
  else Local.Named str
*)

and emit_local ~notice ~need_ref env ((pos, str) as id) =
  if SN.Superglobals.is_superglobal str
  then gather [
    instr_string (SU.Locals.strip_dollar str);
    Emit_pos.emit_pos pos;
    instr (IGet (if need_ref then VGetG else CGetG))
  ]
  else
  let local = get_local env id in
  if is_local_this env str && not (Emit_env.get_needs_local_this env) then
    if need_ref then
      instr_vgetl local
    else
      instr (IMisc (BareThis notice))
  else if need_ref then
    instr_vgetl local
  else
    instr_cgetl local

(* Emit CGetL2 for local variables, and return true to indicate that
 * the result will be just below the top of the stack *)
and emit_first_expr env (_, e as expr) =
  match e with
  | A.Lvar ((_, name) as id)
    when not ((is_local_this env name && not (Emit_env.get_needs_local_this env))
      || SN.Superglobals.is_superglobal name) ->
    instr_cgetl2 (get_local env id), true
  | _ ->
    emit_expr_and_unbox_if_necessary ~need_ref:false env expr, false

(* Special case for binary operations to make use of CGetL2 *)
and emit_two_exprs env outer_pos e1 e2 =
  let instrs1, is_under_top = emit_first_expr env e1 in
  let instrs2 = emit_expr_and_unbox_if_necessary ~need_ref:false env e2 in
  let instrs2_is_var =
    match e2 with
    | _, A.Lvar _ -> true
    | _ -> false in
  gather @@
    if is_under_top
    then
      if instrs2_is_var
      then [Emit_pos.emit_pos outer_pos; instrs2; instrs1]
      else [instrs2; Emit_pos.emit_pos outer_pos; instrs1]
    else
      if instrs2_is_var
      then [instrs1; Emit_pos.emit_pos outer_pos; instrs2]
      else [instrs1; instrs2; Emit_pos.emit_pos outer_pos]

and emit_is_null env e =
  match e with
  | (_, A.Lvar ((_, str) as id)) when not (is_local_this env str) ->
    instr_istypel (get_local env id) OpNull
  | _ ->
    gather [
      emit_expr_and_unbox_if_necessary ~need_ref:false env e;
      instr_istypec OpNull
    ]

and emit_binop env expr op e1 e2 =
  let default () =
    gather [
      emit_two_exprs env (fst expr) e1 e2;
      from_binop op
    ] in
  match op with
  | A.AMpamp | A.BArbar -> emit_short_circuit_op env expr
  | A.Eq None ->
    emit_lval_op env (fst expr) LValOp.Set e1 (Some e2)
  | A.Eq (Some obop) ->
    begin match binop_to_eqop obop with
    | None -> emit_nyi "illegal eq op"
    | Some op -> emit_lval_op env (fst expr) (LValOp.SetOp op) e1 (Some e2)
    end
  | _ ->
    if not (optimize_null_check ())
    then default ()
    else
    match op with
    | A.EQeqeq when snd e2 = A.Null ->
      emit_is_null env e1
    | A.EQeqeq when snd e1 = A.Null ->
      emit_is_null env e2
    | A.Diff2 when snd e2 = A.Null ->
      gather [
        emit_is_null env e1;
        instr_not
      ]
    | A.Diff2 when snd e1 = A.Null ->
      gather [
        emit_is_null env e2;
        instr_not
      ]
    | _ ->
      default ()

and emit_box_if_necessary need_ref instr =
  if need_ref then
    gather [
      instr;
      instr_box
    ]
  else
    instr

and emit_maybe_classname env (p,name) with_string with_instr =
  let from_str s =
    let e_id, _ =
      Hhbc_id.Class.elaborate_id (Emit_env.get_namespace env) (p,s) in
    with_string e_id in
  if SU.is_static name then
    let get_static =
      gather [ instr_fcallbuiltin 0 0 "get_called_class"; instr_unboxr_nop ] in
    with_instr get_static
  else if SU.is_self name || SU.is_parent name then
    let cls = Scope.get_class (Emit_env.get_scope env) in
    match cls with
    | Some c when c.A.c_kind = A.Ctrait ->
      let get_cls =
        if SU.is_self name then instr_self else instr_parent in
      with_instr (gather [get_cls; instr_clsrefname])
    | Some c when SU.is_self name -> from_str (snd c.A.c_name)
    | Some c ->
        begin match c.A.c_extends with
        | (_, A.Happly ((_, parent), _)) :: _ -> from_str parent
        | _ -> from_str name
        end
    | _ -> from_str name
  else from_str name

and emit_instanceof env e1 e2 =
  match (e1, e2) with
  | (_, (_, A.Id id)) ->
    let lhs = emit_expr ~need_ref:false env e1 in
    emit_maybe_classname env id
      (fun id -> gather [ lhs; instr_instanceofd id ])
      (fun instr -> gather [ lhs; instr; instr_instanceof ])
  | _ ->
    gather [
      emit_expr ~need_ref:false env e1;
      emit_expr ~need_ref:false env e2;
      instr_instanceof ]

and emit_is _env _e _h =
  emit_nyi "is expression"

and emit_null_coalesce env e1 e2 =
  let end_label = Label.next_regular () in
  gather [
    emit_quiet_expr env e1;
    instr_dup;
    instr_istypec OpNull;
    instr_not;
    instr_jmpnz end_label;
    instr_popc;
    emit_expr ~need_ref:false env e2;
    instr_label end_label;
  ]

and emit_cast env hint expr =
  let op =
    begin match hint with
    | A.Happly((_, id), []) ->
      let id = String.lowercase_ascii id in
      begin match id with
      | _ when id = SN.Typehints.int
            || id = SN.Typehints.integer -> instr (IOp CastInt)
      | _ when id = SN.Typehints.bool
            || id = SN.Typehints.boolean -> instr (IOp CastBool)
      | _ when id = SN.Typehints.string -> instr (IOp CastString)
      | _ when id = SN.Typehints.object_cast -> instr (IOp CastObject)
      | _ when id = SN.Typehints.array -> instr (IOp CastArray)
      | _ when id = SN.Typehints.real
            || id = SN.Typehints.double
            || id = SN.Typehints.float -> instr (IOp CastDouble)
      | _ when id = "unset" -> gather [ instr_popc; instr_null ]
      | _ -> emit_nyi "cast type"
      end
      (* TODO: unset *)
    | _ ->
      emit_nyi "cast type"
    end in
  gather [
    emit_expr ~need_ref:false env expr;
    op;
  ]

and emit_conditional_expression env etest etrue efalse =
  match etrue with
  | Some etrue ->
    let false_label = Label.next_regular () in
    let end_label = Label.next_regular () in
    let opt_b, jmp_instrs = emit_jmpz_aux env etest false_label in
    gather [
      jmp_instrs;
      (* Don't emit code for true branch if statically we know condition is false *)
      optional (opt_b <> Some false)
        [emit_expr ~need_ref:false env etrue; instr_jmp end_label];
      instr_label false_label;
      (* Don't emit code for false branch if statically we know condition is true *)
      optional (opt_b <> Some true)
        [emit_expr ~need_ref:false env efalse];
      instr_label end_label;
    ]
  | None ->
    let end_label = Label.next_regular () in
    gather [
      emit_expr ~need_ref:false env etest;
      instr_dup;
      instr_jmpnz end_label;
      instr_popc;
      emit_expr ~need_ref:false env efalse;
      instr_label end_label;
    ]

and emit_new env expr args uargs =
  let nargs = List.length args + List.length uargs in
  let cexpr = expr_to_class_expr ~resolve_self:true
    (Emit_env.get_scope env) expr in
  match cexpr with
    (* Special case for statically-known class *)
  | Class_id id ->
    let fq_id, _id_opt =
      Hhbc_id.Class.elaborate_id (Emit_env.get_namespace env) id in
    gather [
      instr_fpushctord nargs fq_id;
      emit_args_and_call env args uargs;
      instr_popr
      ]
  | Class_static ->
    gather [
      instr_fpushctors nargs SpecialClsRef.Static;
      emit_args_and_call env args uargs;
      instr_popr
      ]
  | Class_self ->
    gather [
      instr_fpushctors nargs SpecialClsRef.Self;
      emit_args_and_call env args uargs;
      instr_popr
      ]
  | Class_parent ->
    gather [
      instr_fpushctors nargs SpecialClsRef.Parent;
      emit_args_and_call env args uargs;
      instr_popr
      ]
  | _ ->
    gather [
      emit_load_class_ref env cexpr;
      instr_fpushctor nargs 0;
      emit_args_and_call env args uargs;
      instr_popr
    ]

and emit_new_anon env cls_idx args uargs =
  let nargs = List.length args + List.length uargs in
  gather [
    instr_defcls cls_idx;
    instr_fpushctori nargs cls_idx;
    emit_args_and_call env args uargs;
    instr_popr
    ]

and emit_clone env expr =
  gather [
    emit_expr ~need_ref:false env expr;
    instr_clone;
  ]

and emit_shape env expr fl =
  let p = fst expr in
  let fl =
    List.map fl
             ~f:(fun (fn, e) ->
                   ((p, extract_shape_field_name_pstring fn), e))
  in
  emit_expr ~need_ref:false env (p, A.Darray fl)

and emit_tuple env p es =
  emit_expr ~need_ref:false env (p, A.Varray es)

and emit_inout_call_set env es = Local.scope @@ fun () ->
  let inout_params =
    List.filter_map es
      ~f:(function
          | _, A.Callconv (A.Pinout, e) ->
            begin match snd e with
            | A.Lvar (_, s) -> Some (instr_setl @@ Local.Named s)
            | A.Array_get (base_expr, opt_e) ->
              let base =
                emit_array_get ~no_final:true ~need_ref:false
                  ~mode:MemberOpMode.Define
                  env None QueryOp.InOut base_expr opt_e in
              let mk = get_elem_member_key env 0 opt_e in
              Some (gather [ base; instr_setm 0 mk ]);
            | _ -> None
            end
          | _ -> None)
  in
  if List.length inout_params = 0 then empty else
  let local = Local.get_unnamed_local () in
  gather [
    instr_unboxr;
    Emit_inout_helpers.emit_list_set_for_inout_call local inout_params
  ]

and emit_call_expr ~need_ref env expr =
  let instrs, flavor = emit_flavored_expr env expr in
  gather [
    instrs;
    (* If the instruction has produced a ref then unbox it *)
    if flavor = Flavor.ReturnVal then
      Emit_pos.emit_pos_then (fst expr) @@
      if need_ref then
        instr_boxr
      else
        instr_unboxr
    else
      empty
  ]

and emit_known_class_id env id =
  let fq_id, _ = Hhbc_id.Class.elaborate_id (Emit_env.get_namespace env) id in
  gather [
    instr_string (Hhbc_id.Class.to_raw_string fq_id);
    instr_clsrefgetc;
  ]

and emit_load_class_ref env cexpr =
  match cexpr with
  | Class_static -> instr (IMisc (LateBoundCls 0))
  | Class_parent -> instr (IMisc (Parent 0))
  | Class_self -> instr (IMisc (Self 0))
  | Class_id id -> emit_known_class_id env id
  | Class_unnamed_local l -> instr (IGet (ClsRefGetL (l, 0)))
  | Class_expr expr ->
    begin match snd expr with
    | A.Lvar ((_, id) as pos_id) when id <> SN.SpecialIdents.this ->
      let local = get_local env pos_id in
      instr (IGet (ClsRefGetL (local, 0)))
    | _ ->
      gather [
        emit_expr ~need_ref:false env expr;
        instr_clsrefgetc
      ]
    end

and emit_load_class_const env cexpr id =
  (* TODO(T21932293): HHVM does not match Zend here.
   * Eventually remove this to match PHP7 *)
  match Ast_scope.Scope.get_class (Emit_env.get_scope env) with
  | Some cd when cd.A.c_kind = A.Ctrait
              && cexpr = Class_self
              && SU.is_class id ->
    instr_string @@ SU.strip_global_ns @@ snd cd.A.c_name
  | _ ->
    let load_const =
      if SU.is_class id
      then instr (IMisc (ClsRefName 0))
      else instr (ILitConst (ClsCns (Hhbc_id.Const.from_ast_name id, 0)))
    in
    gather [
      emit_load_class_ref env cexpr;
      load_const
    ]

and emit_class_expr_parts env cexpr prop =
  let load_prop, load_prop_first =
    match prop with
    | _, A.Id (_, id) ->
      instr_string id, true
    | _, A.Lvar (_, id) ->
      instr_string (SU.Locals.strip_dollar id), true
    | _, A.Dollar (_, A.Lvar _ as e) ->
      emit_expr ~need_ref:false env e, false
      (* The outer dollar just says "class property" *)
    | _, A.Dollar e | e ->
      emit_expr ~need_ref:false env e, true

  in
  let load_cls_ref = emit_load_class_ref env cexpr in
  if load_prop_first then load_prop, load_cls_ref
  else load_cls_ref, load_prop

and emit_class_expr env cexpr prop =
  match cexpr with
  | Class_expr ((pos, (A.BracedExpr _ |
                     A.Dollar _ |
                     A.Call _ |
                     A.Lvar (_, "$this") |
                     A.Binop _ |
                     A.Class_get _)) as e) ->
    (* if class is stored as dollar or braced expression (computed dynamically)
       it needs to be stored in unnamed local and eventually cleaned.
       Here we don't use stash_in_local because shape of the code generated
       for class case is different (PopC / UnsetL is the part of try block) *)
    let cexpr_local =
      Local.scope @@ fun () -> emit_expr ~need_ref:false env e in
    Local.scope @@ fun () ->
      let temp = Local.get_unnamed_local () in
      let instrs = emit_class_expr env (Class_unnamed_local temp) prop in
      let fault_label = Label.next_fault () in
      let block =
        instr_try_fault
          fault_label
          (* try block *)
          (gather [
            instr_popc;
            instrs;
            instr_unsetl temp
          ])
          (* fault block *)
          (gather [
            instr_unsetl temp;
            Emit_pos.emit_pos pos;
            instr_unwind ]) in
      gather [
        cexpr_local;
        instr_setl temp;
        block
      ]
  | _ ->
  let cexpr_begin, cexpr_end = emit_class_expr_parts env cexpr prop in
  gather [cexpr_begin ; cexpr_end]

and emit_class_get env param_num_opt qop need_ref cid prop =
  let cexpr = expr_to_class_expr ~resolve_self:false
    (Emit_env.get_scope env) cid
  in
  gather [
    emit_class_expr env cexpr prop;
    match (param_num_opt, qop) with
    | (None, QueryOp.CGet) -> if need_ref then instr_vgets else instr_cgets
    | (None, QueryOp.CGetQuiet) -> failwith "emit_class_get: CGetQuiet"
    | (None, QueryOp.Isset) -> instr_issets
    | (None, QueryOp.Empty) -> instr_emptys
    | (None, QueryOp.InOut) -> failwith "emit_class_get: InOut"
    | (Some (i, h), _) -> instr (ICall (FPassS (i, 0, h)))
  ]

(* Class constant <cid>::<id>.
 * We follow the logic for the Construct::KindOfClassConstantExpression
 * case in emitter.cpp
 *)
and emit_class_const env cid (_, id) =
  let cexpr = expr_to_class_expr ~resolve_self:true
    (Emit_env.get_scope env) cid in
  match cexpr with
  | Class_id cid ->
    let fq_id, _id_opt =
      Hhbc_id.Class.elaborate_id (Emit_env.get_namespace env) cid in
    let fq_id_str = Hhbc_id.Class.to_raw_string fq_id in
    Emit_symbol_refs.add_class fq_id_str;
    if SU.is_class id
    then instr_string fq_id_str
    else instr (ILitConst (ClsCnsD (Hhbc_id.Const.from_ast_name id, fq_id)))
  | _ ->
    emit_load_class_const env cexpr id

and emit_yield env = function
  | A.AFvalue e ->
    gather [
      emit_expr ~need_ref:false env e;
      instr_yield;
    ]
  | A.AFkvalue (e1, e2) ->
    gather [
      emit_expr ~need_ref:false env e1;
      emit_expr ~need_ref:false env e2;
      instr_yieldk;
    ]

and emit_execution_operator env exprs =
  let instrs =
    match exprs with
    (* special handling of ``*)
    | [_, A.String (_, "") as e] -> emit_expr ~need_ref:false env e
    | _ ->  emit_string2 env exprs in
  gather [
    instr_fpushfuncd 1 (Hhbc_id.Function.from_raw_string "shell_exec");
    instrs;
    instr_fpass PassByRefKind.AllowCell 0 Cell;
    instr_fcall 1;
  ]

and emit_string2 env exprs =
  match exprs with
  | [e] ->
    gather [
      emit_expr ~need_ref:false env e;
      instr (IOp CastString)
    ]
  | e1::e2::es ->
    gather @@ [
      emit_two_exprs env (fst e1) e1 e2;
      instr (IOp Concat);
      gather (List.map es (fun e ->
        gather [emit_expr ~need_ref:false env e; instr (IOp Concat)]))
    ]

  | [] -> failwith "String2 with zero arguments is impossible"


and emit_lambda env fundef ids =
  (* Closure conversion puts the class number used for CreateCl in the "name"
   * of the function definition *)
  let fundef_name = snd fundef.A.f_name in
  let class_num = int_of_string fundef_name in
  let explicit_use = SSet.mem fundef_name (Emit_env.get_explicit_use_set ()) in
  gather [
    gather @@ List.map ids
      (fun (x, isref) ->
        instr (IGet (
          let lid = get_local env x in
          if explicit_use
          then
            if isref then VGetL lid else CGetL lid
          else CUGetL lid)));
    instr (IMisc (CreateCl (List.length ids, class_num)))
  ]

and emit_id env (p, s as id) =
  let s = String.uppercase_ascii s in
  match s with
  | "__FILE__" -> instr (ILitConst File)
  | "__DIR__" -> instr (ILitConst Dir)
  | "__METHOD__" -> instr (ILitConst Method)
  | "__LINE__" ->
    (* If the expression goes on multi lines, we return the last line *)
    let _, line, _, _ = Pos.info_pos_extended p in
    instr_int line
  | "__NAMESPACE__" ->
    let ns = Emit_env.get_namespace env in
    instr_string (Option.value ~default:"" ns.Namespace_env.ns_name)
  | "__COMPILER_FRONTEND__" -> instr_string "hackc"
  | ("EXIT" | "DIE") ->
    emit_exit env None
  | _ ->
    let fq_id, id_opt, contains_backslash =
      Hhbc_id.Const.elaborate_id (Emit_env.get_namespace env) id in
    begin match id_opt with
    | Some id ->
      Emit_symbol_refs.add_constant (Hhbc_id.Const.to_raw_string fq_id);
      Emit_symbol_refs.add_constant id;
      instr (ILitConst (CnsU (fq_id, id)))
    | None ->
      Emit_symbol_refs.add_constant (snd id);
      instr (ILitConst
        (if contains_backslash then CnsE fq_id else Cns fq_id))
    end

and rename_xhp (p, s) = (p, SU.Xhp.mangle s)

and emit_xhp env p id attributes children =
  (* Translate into a constructor call. The arguments are:
   *  1) struct-like array of attributes
   *  2) vec-like array of children
   *  3) filename, for debugging
   *  4) line number, for debugging
   *
   *  Spread operators are injected into the attributes array with placeholder
   *  keys that the runtime will interpret as a spread. These keys are not
   *  parseable as user-specified attributes, so they will never collide.
   *)
  let create_spread p id = (p, "...$" ^ string_of_int(id)) in
  let convert_attr (spread_id, attrs) = function
    | A.Xhp_simple (name, v) ->
        let attr = (A.SFlit name, Html_entities.decode_expr v) in
        (spread_id, attr::attrs)
    | A.Xhp_spread e ->
        let (p, _) = e in
        let attr = (A.SFlit (create_spread p spread_id), Html_entities.decode_expr e) in
        (spread_id + 1, attr::attrs) in
  let (_, attributes) = List.fold_left ~f:convert_attr ~init:(0, []) attributes in
  let attribute_map = p, A.Shape (List.rev attributes) in
  let dec_children = List.map ~f:Html_entities.decode_expr children in
  let children_vec = p, A.Varray dec_children in
  let filename = p, A.Id (p, "__FILE__") in
  let line = p, A.Id (p, "__LINE__") in
  let renamed_id = rename_xhp id in
  Emit_symbol_refs.add_class (snd renamed_id);
  emit_expr ~need_ref:false env @@
    (p, A.New (
      (p, A.Id renamed_id),
      [attribute_map ; children_vec ; filename ; line],
      []))

and emit_import env flavor e =
  let import_instr = match flavor with
    | A.Include -> instr @@ IIncludeEvalDefine Incl
    | A.Require -> instr @@ IIncludeEvalDefine Req
    | A.IncludeOnce -> instr @@ IIncludeEvalDefine InclOnce
    | A.RequireOnce -> instr @@ IIncludeEvalDefine ReqOnce
  in
  add_include e;
  gather [
    emit_expr ~need_ref:false env e;
    import_instr;
  ]

and emit_call_isset_expr env outer_pos (pos, expr_ as expr) =
  match expr_ with
  | A.Array_get ((_, A.Lvar (_, x)), Some e) when x = SN.Superglobals.globals ->
    gather [
      emit_expr ~need_ref:false env e;
      Emit_pos.emit_pos outer_pos;
      instr (IIsset IssetG)
    ]
  | A.Array_get (base_expr, opt_elem_expr) ->
    emit_array_get ~need_ref:false env None QueryOp.Isset base_expr opt_elem_expr
  | A.Class_get (cid, id)  ->
    emit_class_get env None QueryOp.Isset false cid id
  | A.Obj_get (expr, prop, nullflavor) ->
    emit_obj_get ~need_ref:false env pos None QueryOp.Isset expr prop nullflavor
  | A.Lvar ((_, name) as id)
    when is_local_this env name && not (Emit_env.get_needs_local_this env) ->
    gather [
      emit_local ~notice:NoNotice ~need_ref:false env id;
      instr_istypec OpNull;
      instr_not
    ]
  | A.Lvar id ->
    instr (IIsset (IssetL (get_local env id)))
  | A.Dollar e ->
    gather [
      emit_expr ~need_ref:false env e;
      instr_issetn
    ]
  | _ ->
    gather [
      emit_expr ~need_ref:false env expr;
      instr_istypec OpNull;
      instr_not
    ]

and emit_call_empty_expr env (pos, expr_ as expr) =
  match expr_ with
  | A.Array_get((_, A.Lvar (_, x)), Some e) when x = SN.Superglobals.globals ->
    gather [
      emit_expr ~need_ref:false env e;
      instr_emptyg
    ]
  | A.Array_get(base_expr, opt_elem_expr) ->
    emit_array_get ~need_ref:false env None QueryOp.Empty base_expr opt_elem_expr
  | A.Class_get (cid, id) ->
    emit_class_get env None QueryOp.Empty false cid id
  | A.Obj_get (expr, prop, nullflavor) ->
    emit_obj_get ~need_ref:false env pos None QueryOp.Empty expr prop nullflavor
  | A.Lvar(_, id) when SN.Superglobals.is_superglobal id ->
    gather [
      instr_string @@ SU.Locals.strip_dollar id;
      instr_emptyg
    ]
  | A.Lvar id when not (is_local_this env (snd id)) ->
    instr_emptyl (get_local env id)
  | A.Dollar e ->
    gather [
      emit_expr ~need_ref:false env e;
      instr_emptyn
    ]
  | _ ->
    gather [
      emit_expr ~need_ref:false env expr;
      instr_not
    ]

and emit_unset_expr env expr =
  emit_lval_op_nonlist env (fst expr) LValOp.Unset expr empty 0

and emit_call_isset_exprs env pos exprs =
  match exprs with
  | [] -> emit_nyi "isset()"
  | [expr] -> emit_call_isset_expr env pos expr
  | _ ->
    let n = List.length exprs in
    let its_done = Label.next_regular () in
      gather [
        gather @@
        List.mapi exprs
        begin fun i expr ->
          gather [
            emit_call_isset_expr env pos expr;
            if i < n-1 then
            gather [
              instr_dup;
              instr_jmpz its_done;
              instr_popc
            ] else empty
          ]
        end;
        instr_label its_done
      ]

and emit_exit env expr_opt =
  gather [
    (match expr_opt with
      | None -> instr_int 0
      | Some e -> emit_expr ~need_ref:false env e);
    instr_exit;
  ]

and emit_idx env es =
  let default = if List.length es = 2 then instr_null else empty in
  gather [
    emit_exprs env es;
    default;
    instr_idx;
  ]

and emit_define env s e =
  gather [
    emit_expr ~need_ref:false env e;
    instr_defcns s;
  ]

and emit_eval env e =
  gather [
    emit_expr ~need_ref:false env e;
    instr_eval;
  ]

and emit_xhp_obj_get_raw env e s nullflavor =
  let p = Pos.none in
  let fn_name = p, A.Obj_get (e, (p, A.Id (p, "getAttribute")), nullflavor) in
  let args = [p, A.String (p, SU.Xhp.clean s)] in
  fst (emit_call env p fn_name args [])

and emit_xhp_obj_get ~need_ref env param_num_opt e s nullflavor =
  let call = emit_xhp_obj_get_raw env e s nullflavor in
  match param_num_opt with
  | Some (i, h) -> gather [ call; instr_fpassr i h ]
  | None -> gather [ call; if need_ref then instr_boxr else instr_unboxr ]

and emit_get_class_no_args () =
  gather [
    instr_fpushfuncd 0 (Hhbc_id.Function.from_raw_string "get_class");
    instr_fcall 0;
    instr_unboxr
  ]

and emit_class_alias es =
  let c1, c2 = match es with
    | (_, A.String (_, c1)) :: (_, A.String (_, c2)) :: _ -> c1, c2
    | _ -> failwith "emit_class_alias: impossible"
  in
  let default = if List.length es = 2 then instr_true else instr_string c2 in
  gather [
    default;
    instr_alias_cls c1 c2
  ]

and emit_await env e =
  let after_await = Label.next_regular () in
  gather [
    emit_expr ~need_ref:false env e;
    instr_dup;
    instr_istypec OpNull;
    instr_jmpnz after_await;
    instr_await;
    instr_label after_await;
  ]

and emit_callconv _env kind _e =
  match kind with
  | A.Pinout ->
    failwith "emit_callconv: This should have been caught at emit_arg"

and emit_inline_hhas s =
  let lexer = Lexing.from_string s in
  try
    let instrs = Hhas_parser.functionbody Hhas_lexer.read lexer in
    (* TODO: handle case when code after inline hhas is unreachable
      i.e. fallthrough return should not be emitted *)
    match get_estimated_stack_depth instrs with
    | 0 -> gather [ instrs; instr_null ]
    | 1 -> instrs
    | _ ->
      Emit_fatal.raise_fatal_runtime Pos.none
        "Inline assembly expressions should leave the stack unchanged, \
        or push exactly one cell onto the stack."
  with Parsing.Parse_error ->
    Emit_fatal.raise_fatal_parse Pos.none "error parsing inline hhas"

and emit_expr env (pos, expr_ as expr) ~need_ref =
  Emit_pos.emit_pos_then pos @@
  match expr_ with
  | A.Float _ | A.String _ | A.Int _ | A.Null | A.False | A.True ->
    let v = Ast_constant_folder.expr_to_typed_value (Emit_env.get_namespace env) expr in
    emit_box_if_necessary need_ref @@ instr (ILitConst (TypedValue v))
  | A.ParenthesizedExpr e ->
    emit_expr ~need_ref env e
  | A.Lvar id ->
    emit_local ~notice:Notice ~need_ref env id
  | A.Class_const (cid, id) ->
    emit_class_const env cid id
  | A.Unop (op, e) ->
    emit_unop ~need_ref env pos op e
  | A.Binop (op, e1, e2) ->
    emit_box_if_necessary need_ref @@ emit_binop env expr op e1 e2
  | A.Pipe (e1, e2) ->
    emit_box_if_necessary need_ref @@ emit_pipe env e1 e2
  | A.InstanceOf (e1, e2) ->
    emit_box_if_necessary need_ref @@ emit_instanceof env e1 e2
  | A.Is (e, h) ->
    emit_box_if_necessary need_ref @@ emit_is env e h
  | A.NullCoalesce (e1, e2) ->
    emit_box_if_necessary need_ref @@ emit_null_coalesce env e1 e2
  | A.Cast((_, hint), e) ->
    emit_box_if_necessary need_ref @@ emit_cast env hint e
  | A.Eif (etest, etrue, efalse) ->
    emit_box_if_necessary need_ref @@
      emit_conditional_expression env etest etrue efalse
  | A.Expr_list es -> gather @@ List.map es ~f:(emit_expr ~need_ref:false env)
  | A.Array_get((_, A.Lvar (_, x)), Some e) when x = SN.Superglobals.globals ->
    gather [
      emit_expr ~need_ref:false env e;
      instr (IGet (if need_ref then VGetG else CGetG))
    ]
  | A.Array_get(base_expr, opt_elem_expr) ->
    let query_op = if need_ref then QueryOp.Empty else QueryOp.CGet in
    emit_array_get ~need_ref env None query_op base_expr opt_elem_expr
  | A.Obj_get (expr, prop, nullflavor) ->
    let query_op = if need_ref then QueryOp.Empty else QueryOp.CGet in
    emit_obj_get ~need_ref env pos None query_op expr prop nullflavor
  | A.Call ((_, A.Id (_, "isset")), _, exprs, []) ->
    emit_box_if_necessary need_ref @@ emit_call_isset_exprs env pos exprs
  | A.Call ((_, A.Id (_, "empty")), _, [expr], []) ->
    emit_box_if_necessary need_ref @@ emit_call_empty_expr env expr
  (* Did you know that tuples are functions? *)
  | A.Call ((p, A.Id (_, "tuple")), _, es, _)
    when Emit_env.is_hh_syntax_enabled () ->
    emit_box_if_necessary need_ref @@ emit_tuple env p es
  | A.Call ((_, A.Id (_, "idx")), _, es, _) ->
    emit_box_if_necessary need_ref @@ emit_idx env es
  | A.Call ((_, A.Id (_, "define")), _, [(_, A.String (_, s)); e], _)
    when is_global_namespace env ->
    emit_box_if_necessary need_ref @@ emit_define env s e
  | A.Call ((_, A.Id (_, "eval")), _, [expr], _) ->
    emit_box_if_necessary need_ref @@ emit_eval env expr
  | A.Call ((_, A.Id (_, "class_alias")), _, es, _) ->
    emit_box_if_necessary need_ref @@ emit_class_alias es
  | A.Call ((_, A.Id (_, "get_class")), _, [], _) ->
    emit_box_if_necessary need_ref @@ emit_get_class_no_args ()
  | A.Call ((_, A.Id (_, ("exit" | "die"))), _, es, _) ->
    emit_exit env (List.hd es)
  | A.Call _
  (* execution operator is compiled as call to `shell_exec` and should
     be handled in the same way *)
  | A.Execution_operator _ ->
    emit_call_expr ~need_ref env expr
  | A.New (typeexpr, args, uargs) ->
    emit_box_if_necessary need_ref @@ emit_new env typeexpr args uargs
  | A.NewAnonClass (args, uargs, { A.c_name = (_, cls_name); _ }) ->
    let cls_idx = int_of_string cls_name in
    emit_box_if_necessary need_ref @@ emit_new_anon env cls_idx args uargs
  | A.Array es ->
    emit_box_if_necessary need_ref @@ emit_collection env expr es
  | A.Darray es ->
    let es2 = List.map ~f:(fun (e1, e2) -> A.AFkvalue (e1, e2)) es in
    let darray_e = fst expr, A.Darray es in
    emit_box_if_necessary need_ref @@ emit_collection env darray_e es2
  | A.Varray es ->
    let es2 = List.map ~f:(fun e -> A.AFvalue e) es in
    let varray_e = fst expr, A.Varray es in
    emit_box_if_necessary need_ref @@ emit_collection env varray_e es2
  | A.Collection ((pos, name), fields) ->
    emit_box_if_necessary need_ref
      @@ emit_named_collection env expr pos name fields
  | A.Clone e ->
    emit_box_if_necessary need_ref @@ emit_clone env e
  | A.Shape fl ->
    emit_box_if_necessary need_ref @@ emit_shape env expr fl
  | A.Await e -> emit_await env e
  | A.Yield e -> emit_yield env e
  | A.Yield_break ->
    failwith "yield break should be in statement position"
  | A.Yield_from _ -> failwith "complex yield_from expression"
  | A.Lfun _ ->
    failwith "expected Lfun to be converted to Efun during closure conversion"
  | A.Efun (fundef, ids) -> emit_lambda env fundef ids
  | A.Class_get (cid, id)  ->
    emit_class_get env None QueryOp.CGet need_ref cid id
  | A.String2 es -> emit_string2 env es
  | A.BracedExpr e -> emit_expr ~need_ref:false env e
  | A.Dollar e ->
    check_non_pipe_local e;
    let instr = emit_expr ~need_ref:false env e in
    if need_ref then
      gather [
        instr;
        instr_vgetn
      ]
    else
      gather [
        instr;
        instr_cgetn
      ]
  | A.Id id -> emit_id env id
  | A.Xml (id, attributes, children) ->
    emit_xhp env (fst expr) id attributes children
  | A.Callconv (kind, e) ->
    emit_box_if_necessary need_ref @@ emit_callconv env kind e
  | A.Import (flavor, e) -> emit_import env flavor e
  | A.Id_type_arguments (id, _) -> emit_id env id
  | A.Omitted -> empty
  | A.Unsafeexpr _ ->
    failwith "Unsafe expression should be removed during closure conversion"
  | A.Suspend _ ->
    failwith "Codegen for 'suspend' operator is not supported"
  | A.List _ ->
    failwith "List destructor can only be used as an lvar"

and emit_static_collection ~transform_to_collection tv =
  let transform_instr =
    match transform_to_collection with
    | Some collection_type -> instr_colfromarray collection_type
    | _ -> empty
  in
  gather [
    instr (ILitConst (TypedValue tv));
    transform_instr;
  ]

and emit_value_only_collection env es constructor =
  let limit =
    Hhbc_options.max_array_elem_size_on_the_stack !Hhbc_options.compiler_options
  in
  let inline exprs =
    gather
    [gather @@ List.map exprs
      ~f:(function
        (* Drop the keys *)
        | A.AFkvalue (_, e)
        | A.AFvalue e -> emit_expr ~need_ref:false env e);
      instr @@ ILitConst (constructor @@ List.length exprs)]
  in
  let outofline exprs =
    gather @@
    List.map exprs
      ~f:(function
        (* Drop the keys *)
        | A.AFkvalue (_, e)
        | A.AFvalue e -> gather [emit_expr ~need_ref:false env e; instr_add_new_elemc])
  in
  match (List.groupi ~break:(fun i _ _ -> i = limit) es) with
    | [] -> empty
    | x1 :: [] -> inline x1
    | x1 :: x2 :: _ -> gather [inline x1; outofline x2]

and emit_keyvalue_collection name env es constructor =
  let name = SU.strip_ns name in
  let transform_instr =
    if name = "dict" || name = "array" then empty else
      let collection_type = collection_type name in
      instr_colfromarray collection_type
  in
  let add_elem_instr =
    if name = "array" then instr_add_new_elemc
    else gather [instr_dup; instr_add_elemc]
  in
  gather [
    instr (ILitConst constructor);
    gather (List.map es ~f:(expr_and_new env add_elem_instr instr_add_elemc));
    transform_instr;
  ]

and emit_struct_array env es ctor =
  let es =
    List.map es
      ~f:(function A.AFkvalue ((_, A.String (_, s)), v) ->
         s, emit_expr ~need_ref:false env v
                 | _ -> failwith "impossible")
  in
  gather [
    gather @@ List.map es ~f:snd;
    ctor @@ List.map es ~f:fst;
  ]

(* isPackedInit() returns true if this expression list looks like an
 * array with no keys and no ref values *)
and is_packed_init ?(hack_arr_compat=true) es =
  let is_only_values =
    List.for_all es ~f:(function A.AFkvalue _ -> false | _ -> true)
  in
  let keys_are_zero_indexed_properly_formed =
    List.foldi es ~init:true ~f:(fun i b f -> b && match f with
      | A.AFkvalue ((_, A.Int (_, k)), _) ->
        int_of_string k = i
      (* arrays with int-like string keys are still considered packed
         and should be emitted via NewArray *)
      | A.AFkvalue ((_, A.String (_, k)), _) when not hack_arr_compat ->
        (try int_of_string k = i with Failure _ -> false)
      | A.AFvalue _ ->
        true
      | _ -> false)
  in
  let has_references =
    (* Reference can only exist as a value *)
    List.exists es
      ~f:(function A.AFkvalue (_, e)
                 | A.AFvalue e -> expr_starts_with_ref e)
  in
  let has_bool_keys =
    List.exists es
      ~f:(function A.AFkvalue ((_, (A.True | A.False)), _) -> true | _ -> false)
  in
  (is_only_values || keys_are_zero_indexed_properly_formed)
  && not (has_bool_keys && (hack_arr_compat && hack_arr_compat_notices()))
  && not has_references
  && (List.length es) > 0

and is_struct_init es allow_numerics =
  let has_references =
    (* Reference can only exist as a value *)
    List.exists es
      ~f:(function A.AFkvalue (_, e)
                 | A.AFvalue e -> expr_starts_with_ref e)
  in
  let keys = ULS.empty in
  let are_all_keys_non_numeric_strings, keys =
    List.fold_right es ~init:(true, keys) ~f:(fun field (b, keys) ->
      match field with
        | A.AFkvalue ((_, A.String (_, s)), _) ->
          b && (Option.is_none
            @@ Typed_value.string_to_int_opt ~allow_following:false s),
          ULS.add keys s
        | _ -> false, keys)
  in
  let num_keys = List.length es in
  let has_duplicate_keys =
    ULS.cardinal keys <> num_keys
  in
  let limit =
    Hhbc_options.max_array_elem_size_on_the_stack !Hhbc_options.compiler_options
  in
  (allow_numerics || are_all_keys_non_numeric_strings)
  && not has_duplicate_keys
  && not has_references
  && num_keys <= limit
  && num_keys != 0

(* transform_to_collection argument keeps track of
 * what collection to transform to *)
and emit_dynamic_collection env expr es =
  let count = List.length es in
  match snd expr with
  | A.Collection ((_, "vec"), _) ->
    emit_value_only_collection env es (fun n -> NewVecArray n)
  | A.Collection ((_, "keyset"), _) ->
    emit_value_only_collection env es (fun n -> NewKeysetArray n)
  | A.Collection ((_, "dict"), _) ->
     if is_struct_init es true then
       emit_struct_array env es instr_newstructdict
     else
       emit_keyvalue_collection "dict" env es (NewDictArray count)
  | A.Collection ((_, name), _)
     when SU.strip_ns name = "Set"
      || SU.strip_ns name = "ImmSet"
      || SU.strip_ns name = "Map"
      || SU.strip_ns name = "ImmMap" ->
     if is_struct_init es true then
       gather [
           emit_struct_array env es instr_newstructdict;
           instr_colfromarray (collection_type (SU.strip_ns name));
         ]
     else
       emit_keyvalue_collection name env es (NewDictArray count)

  | A.Varray _ ->
    emit_value_only_collection env es (fun n -> NewVArray n)
  | A.Darray _ ->
     if is_struct_init es false then
       emit_struct_array env es instr_newstructdarray
     else
       emit_keyvalue_collection "array" env es (NewDArray count)
  | _ ->
  (* From here on, we're only dealing with PHP arrays *)
  if is_packed_init es then
    emit_value_only_collection env es (fun n -> NewPackedArray n)
  else if is_struct_init es false then
    emit_struct_array env es instr_newstructarray
  else if is_packed_init ~hack_arr_compat:false es then
    emit_keyvalue_collection "array" env es (NewArray count)
  else
    emit_keyvalue_collection "array" env es (NewMixedArray count)

and emit_named_collection env expr pos name fields =
  let name = SU.Types.fix_casing @@ SU.strip_ns name in
  match name with
  | "dict" | "vec" | "keyset"
    -> emit_collection env expr fields
  | "Vector" | "ImmVector" ->
    let collection_type = collection_type name in
    if fields = []
    then instr_newcol collection_type
    else
    gather [
      emit_collection env (pos, A.Collection ((pos, "vec"), fields)) fields;
      instr_colfromarray collection_type;
    ]
  | "Map" | "ImmMap" | "Set" | "ImmSet" ->
    let collection_type = collection_type name in
    if fields = []
    then instr_newcol collection_type
    else
      emit_collection
        ~transform_to_collection:collection_type
        env
        expr
        fields
  | "Pair" ->
    gather [
      gather (List.map fields (function
        | A.AFvalue e -> emit_expr ~need_ref:false env e
        | _ -> failwith "impossible Pair argument"));
      instr (ILitConst NewPair);
    ]
  | _ -> failwith @@ "collection: " ^ name ^ " does not exist"

and is_php_array = function
 | _, A.Array _ -> true
 | _, A.Varray _ -> true
 | _, A.Darray _ -> true
 | _ -> false

and emit_collection ?(transform_to_collection) env expr es =
  match Ast_constant_folder.expr_to_opt_typed_value
          ~allow_maps:true
          ~restrict_keys:(not @@ is_php_array expr)
          (Emit_env.get_namespace env)
          expr
  with
  | Some tv ->
    emit_static_collection ~transform_to_collection tv
  | None ->
    emit_dynamic_collection env expr es

and emit_pipe env e1 e2 =
  stash_in_local ~always_stash:true env e1
  begin fun temp _break_label ->
  let env = Emit_env.with_pipe_var temp env in
  emit_expr ~need_ref:false env e2
  end

(* Emit code that is equivalent to
 *   <code for expr>
 *   JmpZ label
 * Generate specialized code in case expr is statically known, and for
 * !, && and || expressions
 *)
and emit_jmpz_aux env (pos, expr_ as expr) label =
  let opt = optimize_null_check () in
  match Ast_constant_folder.expr_to_opt_typed_value (Emit_env.get_namespace env) expr with
  | Some v ->
    let b = Typed_value.to_bool v in
    Some b, Emit_pos.emit_pos_then pos @@
      (if b then empty else instr_jmp label)
  | None ->
    None,
    Emit_pos.emit_pos_then pos @@
    begin match expr_ with
    | A.Unop(A.Unot, e) ->
      emit_jmpnz env e label
    | A.Binop(A.BArbar, e1, e2) ->
      let skip_label = Label.next_regular () in
      gather [
        emit_jmpnz env e1 skip_label;
        emit_jmpz env e2 label;
        instr_label skip_label;
      ]
    | A.Binop(A.AMpamp, e1, e2) ->
      gather [
        emit_jmpz env e1 label;
        emit_jmpz env e2 label;
      ]
    | A.Binop(A.EQeqeq, e, (_, A.Null))
    | A.Binop(A.EQeqeq, (_, A.Null), e) when opt ->
      gather [
        emit_is_null env e;
        instr_jmpz label
      ]
    | A.Binop(A.Diff2, e, (_, A.Null))
    | A.Binop(A.Diff2, (_, A.Null), e) when opt ->
      gather [
        emit_is_null env e;
        instr_jmpnz label
      ]
    | _ ->
      gather [
        emit_expr ~need_ref:false env expr;
        instr_jmpz label
      ]
    end

and emit_jmpz env expr label =
  snd (emit_jmpz_aux env expr label)

(* Emit code that is equivalent to
 *   <code for expr>
 *   JmpNZ label
 * Generate specialized code in case expr is statically known, and for
 * !, && and || expressions
 *)
and emit_jmpnz env (pos, expr_ as expr) label =
  let opt = optimize_null_check () in
  Emit_pos.emit_pos_then pos @@
  match Ast_constant_folder.expr_to_opt_typed_value (Emit_env.get_namespace env) expr with
  | Some v ->
    if Typed_value.to_bool v then instr_jmp label else empty
  | None ->
    match expr_ with
    | A.Unop(A.Unot, e) ->
      emit_jmpz env e label
    | A.Binop(A.BArbar, e1, e2) ->
      gather [
        emit_jmpnz env e1 label;
        emit_jmpnz env e2 label;
      ]
    | A.Binop(A.AMpamp, e1, e2) ->
      let skip_label = Label.next_regular () in
      gather [
        emit_jmpz env e1 skip_label;
        emit_jmpnz env e2 label;
        instr_label skip_label;
      ]
    | A.Binop(A.EQeqeq, e, (_, A.Null))
    | A.Binop(A.EQeqeq, (_, A.Null), e) when opt ->
      gather [
        emit_is_null env e;
        instr_jmpnz label
      ]
    | A.Binop(A.Diff2, e, (_, A.Null))
    | A.Binop(A.Diff2, (_, A.Null), e) when opt ->
      gather [
        emit_is_null env e;
        instr_jmpz label
      ]
    | _ ->
      gather [
        emit_expr ~need_ref:false env expr;
        instr_jmpnz label
      ]

and emit_short_circuit_op env expr =
  let its_true = Label.next_regular () in
  let its_done = Label.next_regular () in
  gather [
    emit_jmpnz env expr its_true;
    instr_false;
    instr_jmp its_done;
    instr_label its_true;
    instr_true;
    instr_label its_done ]

and emit_quiet_expr env (pos, expr_ as expr) =
  match expr_ with
  | A.Lvar ((_, name) as id) when not (is_local_this env name) ->
    instr_cgetquietl (get_local env id)
  | A.Dollar e ->
    gather [
      emit_expr ~need_ref:false env e;
      instr_cgetquietn
    ]
  | A.Array_get((_, A.Lvar (_, x)), Some e) when x = SN.Superglobals.globals ->
    gather [
      emit_expr ~need_ref:false env e;
      instr (IGet CGetQuietG)
    ]
  | A.Array_get(base_expr, opt_elem_expr) ->
    emit_array_get ~need_ref:false env None QueryOp.CGetQuiet base_expr opt_elem_expr
  | A.Obj_get (expr, prop, nullflavor) ->
    emit_obj_get ~need_ref:false env pos None QueryOp.CGetQuiet expr prop nullflavor
  | _ ->
    emit_expr ~need_ref:false env expr

(* Emit code for e1[e2] or isset(e1[e2]).
 * If param_num_opt = Some i
 * then this is the i'th parameter to a function
 *)
and emit_array_get ?(no_final=false) ?mode ~need_ref
  env param_num_hint_opt qop base_expr opt_elem_expr =
  (* Disallow use of array(..)[] *)
  match base_expr, opt_elem_expr with
  | (pos, A.Array _), None ->
    Emit_fatal.raise_fatal_parse pos "Can't use array() as base in write context"
  | _ ->
  let param_num_hint_opt =
    if qop = QueryOp.InOut then None else param_num_hint_opt in
  let mode = Option.value mode ~default:(get_queryMOpMode need_ref qop) in
  let elem_expr_instrs, elem_stack_size = emit_elem_instrs env opt_elem_expr in
  let param_num_opt = Option.map ~f:(fun (n, _h) -> n) param_num_hint_opt in
  let base_expr_instrs_begin,
      base_expr_instrs_end,
      base_setup_instrs,
      base_stack_size =
    emit_base ~is_object:false
      ~notice:(match qop with QueryOp.Isset -> NoNotice | _ -> Notice)
      env mode elem_stack_size param_num_opt base_expr
  in
  let mk = get_elem_member_key env 0 opt_elem_expr in
  let total_stack_size = elem_stack_size + base_stack_size in
  let final_instr = if no_final then empty else
    instr (IFinal (
      match param_num_hint_opt with
      | None ->
        if need_ref then
          VGetM (total_stack_size, mk)
        else
          QueryM (total_stack_size, qop, mk)
      | Some (i, h) -> FPassM (i, total_stack_size, mk, h)
    )) in
  gather [
    base_expr_instrs_begin;
    elem_expr_instrs;
    base_expr_instrs_end;
    base_setup_instrs;
    final_instr
  ]

(* Emit code for e1->e2 or e1?->e2 or isset(e1->e2).
 * If param_num_opt = Some i
 * then this is the i'th parameter to a function
 *)
and emit_obj_get ~need_ref env pos param_num_hint_opt qop expr prop null_flavor =
  match snd expr with
  | A.Lvar (pos, id)
    when id = SN.SpecialIdents.this && null_flavor = A.OG_nullsafe ->
    Emit_fatal.raise_fatal_parse
      pos "?-> is not allowed with $this"
  | _ ->
    begin match snd prop with
    | A.Id (_, s) when SU.Xhp.is_xhp s ->
      emit_xhp_obj_get ~need_ref env param_num_hint_opt expr s null_flavor
    | _ ->
      let param_num_opt = Option.map ~f:(fun (n, _h) -> n) param_num_hint_opt in
      let mode = get_queryMOpMode need_ref qop in
      let mk, prop_expr_instrs, prop_stack_size =
        emit_prop_expr env null_flavor 0 prop in
      let base_expr_instrs_begin,
          base_expr_instrs_end,
          base_setup_instrs,
          base_stack_size =
        emit_base
          ~is_object:true ~notice:Notice
          env mode prop_stack_size param_num_opt expr
      in
      let total_stack_size = prop_stack_size + base_stack_size in
      let final_instr =
        instr (IFinal (
          match param_num_hint_opt with
          | None ->
            if need_ref then
              VGetM (total_stack_size, mk)
            else
              QueryM (total_stack_size, qop, mk)
          | Some (i, h) -> FPassM (i, total_stack_size, mk, h)
        )) in
      gather [
        base_expr_instrs_begin;
        prop_expr_instrs;
        base_expr_instrs_end;
        Emit_pos.emit_pos pos;
        base_setup_instrs;
        final_instr
      ]
    end

and is_special_class_constant_accessed_with_class_id env (_, cName) id =
  (* TODO(T21932293): HHVM does not match Zend here.
   * Eventually remove this to match PHP7 *)
  SU.is_class id &&
  (not (SU.is_self cName || SU.is_parent cName || SU.is_static cName)
  || (Ast_scope.Scope.is_in_trait (Emit_env.get_scope env)) && SU.is_self cName)

and emit_elem_instrs env opt_elem_expr =
  match opt_elem_expr with
  (* These all have special inline versions of member keys *)
  | Some (_, (A.Int _ | A.String _)) -> empty, 0
  | Some (_, (A.Lvar (_, id))) when not (is_local_this env id) -> empty, 0
  | Some (_, (A.Class_const ((_, A.Id cid), (_, id))))
    when is_special_class_constant_accessed_with_class_id env cid id -> empty, 0
  | Some expr -> emit_expr ~need_ref:false env expr, 1
  | None -> empty, 0

(* Get the member key for an array element expression: the `elem` in
 * expressions of the form `base[elem]`.
 * If the array element is missing, use the special key `W`.
 *)
and get_elem_member_key env stack_index opt_expr =
  match opt_expr with
  (* Special case for local *)
  | Some (_, A.Lvar id) when not (is_local_this env (snd id)) ->
    MemberKey.EL (get_local env id)
  (* Special case for literal integer *)
  | Some (_, A.Int (_, str) as int_expr)->
    let open Ast_constant_folder in
    let namespace = Emit_env.get_namespace env in
    begin match expr_to_typed_value namespace int_expr with
    | TV.Int i -> MemberKey.EI i
    | _ -> failwith (str ^ " is not a valid integer index")
    end
  (* Special case for literal string *)
  | Some (_, A.String (_, str)) -> MemberKey.ET str
  (* Special case for class name *)
  | Some (_, (A.Class_const ((_, A.Id (_, cName as cid)), (_, id))))
    when is_special_class_constant_accessed_with_class_id env cid id ->
    (* Special case for self::class in traits *)
    (* TODO(T21932293): HHVM does not match Zend here.
     * Eventually remove this to match PHP7 *)
    let cName =
      match SU.is_self cName,
            SU.is_class id,
            Ast_scope.Scope.get_class (Emit_env.get_scope env)
      with
      | true, true, Some cd -> SU.strip_global_ns @@ snd cd.A.c_name
      | _ -> cName
    in
    MemberKey.ET cName
  (* General case *)
  | Some _ -> MemberKey.EC stack_index
  (* ELement missing (so it's array append) *)
  | None -> MemberKey.W

(* Get the member key for a property, and return any instructions and
 * the size of the stack in the case that the property cannot be
 * placed inline in the instruction. *)
and emit_prop_expr env null_flavor stack_index prop_expr =
  let mk =
    match snd prop_expr with
    | A.Id ((_, name) as id) when String_utils.string_starts_with name "$" ->
      MemberKey.PL (get_local env id)
    (* Special case for known property name *)
    | A.Id (_, id)
    | A.String (_, id) ->
      let pid = Hhbc_id.Prop.from_ast_name id in
      begin match null_flavor with
      | Ast.OG_nullthrows -> MemberKey.PT pid
      | Ast.OG_nullsafe -> MemberKey.QT pid
      end
    | A.Lvar ((_, name) as id) when not (is_local_this env name) ->
      MemberKey.PL (get_local env id)
    (* General case *)
    | _ ->
      MemberKey.PC stack_index
  in
  (* For nullsafe access, insist that property is known *)
  begin match mk with
  | MemberKey.PL _ | MemberKey.PC _ ->
    if null_flavor = A.OG_nullsafe then
      Emit_fatal.raise_fatal_parse (fst prop_expr)
        "?-> can only be used with scalar property names"
  | _ -> ()
  end;
  match mk with
  | MemberKey.PC _ ->
    mk, emit_expr ~need_ref:false env prop_expr, 1
  | _ ->
    mk, empty, 0

(* Emit code for a base expression `expr` that forms part of
 * an element access `expr[elem]` or field access `expr->fld`.
 * The instructions are divided into three sections:
 *   1. base and element/property expression instructions:
 *      push non-trivial base and key values on the stack
 *   2. base selector instructions: a sequence of Base/Dim instructions that
 *      actually constructs the base address from "member keys" that are inlined
 *      in the instructions, or pulled from the key values that
 *      were pushed on the stack in section 1.
 *   3. (constructed by the caller) a final accessor e.g. QueryM or setter
 *      e.g. SetOpM instruction that has the final key inlined in the
 *      instruction, or pulled from the key values that were pushed on the
 *      stack in section 1.
 * The function returns a triple (base_instrs, base_setup_instrs, stack_size)
 * where base_instrs is section 1 above, base_setup_instrs is section 2, and
 * stack_size is the number of values pushed onto the stack by section 1.
 *
 * For example, the r-value expression $arr[3][$ix+2]
 * will compile to
 *   # Section 1, pushing the value of $ix+2 on the stack
 *   Int 2
 *   CGetL2 $ix
 *   AddO
 *   # Section 2, constructing the base address of $arr[3]
 *   BaseL $arr Warn
 *   Dim Warn EI:3
 *   # Section 3, indexing the array using the value at stack position 0 (EC:0)
 *   QueryM 1 CGet EC:0
 *)
and emit_base ~is_object ~notice env mode base_offset param_num_opt (pos, expr_ as expr) =
   let base_mode =
    if mode = MemberOpMode.InOut then MemberOpMode.Warn else mode in
   match expr_ with
   | A.Lvar (_, x) when SN.Superglobals.is_superglobal x ->
     instr_string (SU.Locals.strip_dollar x),
     empty,
     instr (IBase (
     match param_num_opt with
     | None -> BaseGC (base_offset, base_mode)
     | Some i -> FPassBaseGC (i, base_offset)
     )),
     1

   | A.Lvar (thispos, x) when is_object && x = SN.SpecialIdents.this ->
     Emit_pos.emit_pos_then thispos @@ instr (IMisc CheckThis),
     empty,
     instr (IBase BaseH),
     0

   | A.Lvar ((_, str) as id)
     when not (is_local_this env str) || Emit_env.get_needs_local_this env ->
     let v = get_local env id in
     empty,
     empty,
     instr (IBase (
       match param_num_opt with
       | None -> BaseL (v, base_mode)
       | Some i -> FPassBaseL (i, v)
       )),
     0

   | A.Lvar id ->
     emit_local ~notice ~need_ref:false env id,
     empty,
     instr (IBase (BaseC base_offset)),
     1

   | A.Array_get((_, A.Lvar (_, x)), Some (_, A.Lvar y))
     when x = SN.Superglobals.globals ->
     let v = get_local env y in
     empty,
     empty,
     instr (IBase (
       match param_num_opt with
       | None -> BaseGL (v, base_mode)
       | Some i -> FPassBaseGL (i, v)
       )),
     0

   | A.Array_get((_, A.Lvar (_, x)), Some e) when x = SN.Superglobals.globals ->
     let elem_expr_instrs = emit_expr ~need_ref:false env e in
     elem_expr_instrs,
     empty,
     instr (IBase (
     match param_num_opt with
     | None -> BaseGC (base_offset, base_mode)
     | Some i -> FPassBaseGC (i, base_offset)
     )),
   1

   | A.Array_get(base_expr, opt_elem_expr) ->
     let elem_expr_instrs, elem_stack_size = emit_elem_instrs env opt_elem_expr in
     let base_expr_instrs_begin,
         base_expr_instrs_end,
         base_setup_instrs,
         base_stack_size =
       emit_base
         ~notice ~is_object:false
         env mode (base_offset + elem_stack_size) param_num_opt base_expr
     in
     let mk = get_elem_member_key env base_offset opt_elem_expr in
     let total_stack_size = base_stack_size + elem_stack_size in
     gather [
       base_expr_instrs_begin;
       elem_expr_instrs;
     ],
     base_expr_instrs_end,
     gather [
       base_setup_instrs;
       Emit_pos.emit_pos pos;
       instr (IBase (
         match param_num_opt with
         | None -> Dim (mode, mk)
         | Some i -> FPassDim (i, mk)
       ))
     ],
     total_stack_size

   | A.Obj_get(base_expr, prop_expr, null_flavor) ->
     begin match snd prop_expr with
     | A.Id (_, s) when SU.Xhp.is_xhp s ->
       emit_xhp_obj_get_raw env base_expr s null_flavor,
       empty,
       gather [ instr_baser base_offset ],
       1
     | _ ->
       let mk, prop_expr_instrs, prop_stack_size =
         emit_prop_expr env null_flavor base_offset prop_expr in
       let base_expr_instrs_begin,
           base_expr_instrs_end,
           base_setup_instrs,
           base_stack_size =
         emit_base ~notice:Notice ~is_object:true
           env mode (base_offset + prop_stack_size) param_num_opt base_expr
       in
       let total_stack_size = prop_stack_size + base_stack_size in
       let final_instr =
         instr (IBase (
           match param_num_opt with
           | None -> Dim (mode, mk)
           | Some i -> FPassDim (i, mk)
         )) in
       gather [
         base_expr_instrs_begin;
         prop_expr_instrs;
       ],
       base_expr_instrs_end,
       gather [
         base_setup_instrs;
         Emit_pos.emit_pos pos;
         final_instr
       ],
       total_stack_size
     end

   | A.Class_get(cid, (_, A.Dollar (_, A.Lvar id))) ->
     let cexpr = expr_to_class_expr ~resolve_self:false
       (Emit_env.get_scope env) cid in
     (* special case for $x->$$y: use BaseSL *)
     emit_load_class_ref env cexpr,
     empty,
     Emit_pos.emit_pos_then pos @@
     instr_basesl (get_local env id),
     0
   | A.Class_get(cid, prop) ->
     let cexpr = expr_to_class_expr ~resolve_self:false
       (Emit_env.get_scope env) cid in
     let cexpr_begin, cexpr_end = emit_class_expr_parts env cexpr prop in
     cexpr_begin,
     cexpr_end,
     Emit_pos.emit_pos_then pos @@
     instr_basesc base_offset,
     1
   | A.Dollar (_, A.Lvar id as e) ->
     check_non_pipe_local e;
     empty,
     empty,
     Emit_pos.emit_pos_then pos @@
     instr_basenl (get_local env id) base_mode,
     0
   | A.Dollar e ->
     let base_expr_instrs = emit_expr ~need_ref:false env e in
     base_expr_instrs,
     empty,
     Emit_pos.emit_pos_then pos @@
     instr_basenc base_offset base_mode,
     1
   | _ ->
     let base_expr_instrs, flavor = emit_flavored_expr env expr in
     (if binary_assignment_rhs_starts_with_ref expr
     then gather [base_expr_instrs; instr_unbox]
     else base_expr_instrs),
     empty,
     Emit_pos.emit_pos_then pos @@
     instr (IBase (if flavor = Flavor.ReturnVal
                   then BaseR base_offset else BaseC base_offset)),
     1

and get_pass_by_ref_hint expr =
  let with_ref = expr_starts_with_ref expr in
  if Emit_env.is_hh_syntax_enabled ()
  then if with_ref then Ref else Cell
  else Any

and strip_ref e =
  match snd e with
  | A.Unop (A.Uref, e) -> e
  | _ -> e

and emit_arg env i is_splatted expr =
  let is_inout, (pos, _ as expr) =
    match snd expr with
    | A.Callconv (A.Pinout, e) -> true, e
    | _ -> false, expr
  in
  let hint = get_pass_by_ref_hint expr in
  let _, expr_ = strip_ref expr in
  let default () =
    let instrs, flavor = emit_flavored_expr env expr in
    let instrs =
      if is_splatted && flavor = Flavor.ReturnVal
      then gather [ instrs; instr_unboxr ] else instrs
    in
    let fpass_kind =
      match is_splatted, flavor with
      | false, Flavor.Ref -> instr_fpassv i hint
      | false, Flavor.ReturnVal -> instr_fpassr i hint
      | false, Flavor.Cell
      | true, _ -> instr_fpass (get_passByRefKind is_splatted expr) i hint
    in
    gather [
      instrs;
      Emit_pos.emit_pos pos;
      fpass_kind;
    ] in
  if is_splatted then default ()
  else
  match expr_ with
  | A.Lvar (_, x) when SN.Superglobals.is_superglobal x ->
    gather [
      instr_string (SU.Locals.strip_dollar x);
      instr_fpassg i hint
    ]
  | A.Lvar _ when is_inout ->
    gather [
      emit_expr ~need_ref:false env expr;
      Emit_pos.emit_pos pos;
      instr_fpassc i hint;
    ]
  | A.Lvar ((_, str) as id)
    when not (is_local_this env str) || Emit_env.get_needs_local_this env ->
    instr_fpassl i (get_local env id) hint
  | A.BracedExpr e ->
    emit_expr ~need_ref:false env e;
  | A.Dollar e ->
    check_non_pipe_local e;
    gather [
      emit_expr ~need_ref:false env e;
      instr_fpassn i hint;
    ]
  | A.Array_get ((_, A.Lvar (_, x)), Some e) when x = SN.Superglobals.globals ->
    gather [
      emit_expr ~need_ref:false env e;
      instr_fpassg i hint
    ]

  | A.Array_get (base_expr, opt_elem_expr) ->
    let qop = if is_inout then QueryOp.InOut else QueryOp.CGet in
    let instrs =
      emit_array_get
        ~need_ref:false env (Some (i, hint)) qop base_expr opt_elem_expr in
    if is_inout then gather [ instrs; instr_fpassc i hint ] else instrs

  | A.Obj_get (e1, e2, nullflavor) ->
    emit_obj_get ~need_ref:false env pos (Some (i, hint)) QueryOp.CGet e1 e2 nullflavor

  | A.Class_get (cid, e) ->
    emit_class_get env (Some (i, hint)) QueryOp.CGet false cid e

  | A.Binop (A.Eq None, (_, A.List _ as e), (_, A.Lvar id)) ->
    let local = get_local env id in
    let lhs_instrs, set_instrs =
      emit_lval_op_list env (Some local) [] e in
    gather [
      lhs_instrs;
      set_instrs;
      instr_fpassl i local hint;
    ]
  | A.Call _ when expr_starts_with_ref expr ->
    let instrs, _ = emit_flavored_expr env (pos, expr_) in
    gather [
      instrs;
      instr_fpassr i hint;
    ]
  | _ -> default ()

and emit_ignored_expr env ?(pop_pos = Pos.none) e =
  match snd e with
  | A.Expr_list es -> gather @@ List.map ~f:(emit_ignored_expr env ~pop_pos) es
  | _ ->
    let instrs, flavor = emit_flavored_expr env e in
    gather [
      instrs;
      Emit_pos.emit_pos_then pop_pos @@ instr_pop flavor;
    ]

(* Emit code to construct the argument frame and then make the call *)
and emit_args_and_call env args uargs =
  let args_count = List.length args in
  let all_args = args @ uargs in
  let is_splatted =  not (List.is_empty uargs) in
  let nargs = List.length all_args in
  gather [
    gather (List.mapi all_args (fun i e -> emit_arg env i (i >= args_count) e));
    if uargs = [] && not is_splatted
    then instr (ICall (FCall nargs))
    else instr (ICall (FCallUnpack nargs));
    emit_inout_call_set env args
  ]

(* Expression that appears in an object context, such as expr->meth(...) *)
and emit_object_expr env (_, expr_ as expr) =
  match expr_ with
  | A.Lvar(_, x) when is_local_this env x ->
    instr_this
  | _ -> emit_expr ~need_ref:false env expr

and emit_call_lhs_with_this env instrs = Local.scope @@ fun () ->
  let id = Pos.none, SN.SpecialIdents.this in
  let temp = Local.get_unnamed_local () in
  gather [
    emit_local ~notice:Notice ~need_ref:false env id;
    instr_setl temp;
    with_temp_local temp
    begin fun temp _ -> gather [
      instr_popc;
      instrs;
      instr (IGet (ClsRefGetL (temp, 0)));
      instr_unsetl temp;
    ]
    end
  ]

and has_inout_args es =
  List.exists es ~f:(function _, A.Callconv (A.Pinout, _) -> true | _ -> false)

and emit_call_lhs env (_, expr_ as expr) nargs has_splat inout_arg_positions =
  let has_inout_args = List.length inout_arg_positions <> 0 in
  match expr_ with
  | A.Obj_get (obj, (_, A.Id ((_, str) as id)), null_flavor)
    when str.[0] = '$' ->
    gather [
      emit_object_expr env obj;
      instr_cgetl (get_local env id);
      instr_fpushobjmethod nargs null_flavor inout_arg_positions;
    ]
  | A.Obj_get (obj, (_, A.String (_, id)), null_flavor)
  | A.Obj_get (obj, (_, A.Id (_, id)), null_flavor) ->
    let name = Hhbc_id.Method.from_ast_name id in
    let name =
      if has_inout_args
      then Hhbc_id.Method.add_suffix name
        (Emit_inout_helpers.inout_suffix inout_arg_positions)
      else name in
    gather [
      emit_object_expr env obj;
      instr_fpushobjmethodd nargs name null_flavor;
    ]
  | A.Obj_get(obj, method_expr, null_flavor) ->
    gather [
      emit_object_expr env obj;
      emit_expr ~need_ref:false env method_expr;
      instr_fpushobjmethod nargs null_flavor inout_arg_positions;
    ]

  | A.Class_const (cid, (_, id)) ->
    let cexpr = expr_to_class_expr ~resolve_self:false
      (Emit_env.get_scope env) cid in
    let method_id = Hhbc_id.Method.from_ast_name id in
    let method_id =
      if has_inout_args
      then Hhbc_id.Method.add_suffix method_id
        (Emit_inout_helpers.inout_suffix inout_arg_positions)
      else method_id in
    begin match cexpr with
    (* Statically known *)
    | Class_id cid ->
      let fq_cid, _ = Hhbc_id.Class.elaborate_id (Emit_env.get_namespace env) cid in
      instr_fpushclsmethodd nargs method_id fq_cid
    | Class_static -> instr_fpushclsmethodsd nargs SpecialClsRef.Static method_id
    | Class_self -> instr_fpushclsmethodsd nargs SpecialClsRef.Self method_id
    | Class_parent -> instr_fpushclsmethodsd nargs SpecialClsRef.Parent method_id
    | Class_expr (_, A.Lvar (_, x)) when x = SN.SpecialIdents.this ->
       let method_name = Hhbc_id.Method.to_raw_string method_id in
       gather [
         emit_call_lhs_with_this env @@ instr_string method_name;
         instr_fpushclsmethod nargs []
       ]
    | _ ->
       let method_name = Hhbc_id.Method.to_raw_string method_id in
       gather [
         emit_class_expr env cexpr (Pos.none, A.Id (Pos.none, method_name));
         instr_fpushclsmethod nargs []
       ]
    end

  | A.Class_get (cid, e) ->
    let cexpr = expr_to_class_expr ~resolve_self:false
      (Emit_env.get_scope env) cid in
    let expr_instrs = emit_expr ~need_ref:false env e in
    begin match cexpr with
    | Class_static ->
       gather [expr_instrs; instr_fpushclsmethods nargs SpecialClsRef.Static]
    | Class_self ->
       gather [expr_instrs; instr_fpushclsmethods nargs SpecialClsRef.Self]
    | Class_parent ->
       gather [expr_instrs; instr_fpushclsmethods nargs SpecialClsRef.Parent]
    | Class_expr (_, A.Lvar (_, x)) when x = SN.SpecialIdents.this ->
       gather [
        emit_call_lhs_with_this env expr_instrs;
        instr_fpushclsmethod nargs inout_arg_positions
       ]
    | _ ->
       gather [
        expr_instrs;
        emit_load_class_ref env cexpr;
        instr_fpushclsmethod nargs inout_arg_positions
       ]
    end

  | A.Id (_, s as id)->
    let fq_id, id_opt =
      Hhbc_id.Function.elaborate_id_with_builtins (Emit_env.get_namespace env) id in
    let fq_id, id_opt =
      match id_opt, SU.strip_global_ns s with
      | None, "min" when nargs = 2 && not has_splat ->
        Hhbc_id.Function.from_raw_string "__SystemLib\\min2", None
      | None, "max" when nargs = 2 && not has_splat ->
        Hhbc_id.Function.from_raw_string  "__SystemLib\\max2", None
      | _ -> fq_id, id_opt in
    let fq_id = if has_inout_args
      then Hhbc_id.Function.add_suffix
        fq_id (Emit_inout_helpers.inout_suffix inout_arg_positions)
      else fq_id in
    begin match id_opt with
    | Some id -> instr (ICall (FPushFuncU (nargs, fq_id, id)))
    | None -> instr (ICall (FPushFuncD (nargs, fq_id)))
    end
  | A.String (_, s) ->
    instr_fpushfuncd nargs (Hhbc_id.Function.from_raw_string s)
  | _ ->
    gather [
      emit_expr ~need_ref:false env expr;
      instr_fpushfunc nargs inout_arg_positions
    ]

(* Retuns whether the function is a call_user_func function,
  min args, max args *)
and get_call_user_func_info = function
  | "call_user_func" -> (true, 1, max_int)
  | "call_user_func_array" -> (true, 2, 2)
  | "forward_static_call" -> (true, 1, max_int)
  | "forward_static_call_array"  -> (true, 2, 2)
  | "fb_call_user_func_safe" -> (true, 1, max_int)
  | "fb_call_user_func_array_safe" -> (true, 2, 2)
  | "fb_call_user_func_safe_return" -> (true, 2, max_int)
  | _ -> (false, 0, 0)

and is_call_user_func id num_args =
  let (is_fn, min_args, max_args) = get_call_user_func_info id in
  is_fn && num_args >= min_args && num_args <= max_args

and get_call_builtin_func_info lower_fq_id =
  match lower_fq_id with
  | "array_key_exists" -> Some (2, IMisc AKExists)
  | "hphp_array_idx" -> Some (3, IMisc ArrayIdx)
  | "intval" -> Some (1, IOp CastInt)
  | "boolval" -> Some (1, IOp CastBool)
  | "strval" -> Some (1, IOp CastString)
  | "floatval" | "doubleval" -> Some (1, IOp CastDouble)
  | "hh\\vec" -> Some (1, IOp CastVec)
  | "hh\\keyset" -> Some (1, IOp CastKeyset)
  | "hh\\dict" -> Some (1, IOp CastDict)
  | "hh\\varray" -> Some (1, IOp CastVArray)
  | "hh\\darray" -> Some (1, IOp CastDArray)
  | _ -> None

and emit_call_user_func_arg env f i expr =
  let hint = get_pass_by_ref_hint expr in
  let hint, warning, expr =
    if hint = Ref
    then
      (* for warning - adjust the argument id *)
      let param_id = Param_unnamed (i + 1) in
      (* emitter.cpp:
         The passthrough type of call_user_func is always cell or any, so
         any call to a function taking a ref will result in a warning *)
      Cell, instr_raise_fpass_warning hint f param_id, strip_ref expr
    else hint, empty, expr in
  gather [
    emit_expr ~need_ref:false env expr;
    warning;
    instr_fpass PassByRefKind.AllowCell i hint;
  ]

and emit_call_user_func env id arg args =
  let return_default, args = match id with
    | "fb_call_user_func_safe_return" ->
      begin match args with
        | [] -> failwith "fb_call_user_func_safe_return - requires default arg"
        | a :: args -> emit_expr ~need_ref:false env a, args
      end
    | _ -> empty, args
  in
  let num_params = List.length args in
  let begin_instr = match id with
    | "forward_static_call"
    | "forward_static_call_array" -> instr_fpushcuff num_params
    | "fb_call_user_func_safe"
    | "fb_call_user_func_array_safe" ->
      gather [instr_null; instr_fpushcuf_safe num_params]
    | "fb_call_user_func_safe_return" ->
      gather [return_default; instr_fpushcuf_safe num_params]
    | _ -> instr_fpushcuf num_params
  in
  let call_instr = match id with
    | "call_user_func_array"
    | "forward_static_call_array"
    | "fb_call_user_func_array_safe" -> instr (ICall FCallArray)
    | _ -> instr (ICall (FCall num_params))
  in
  let end_instr = match id with
    | "fb_call_user_func_safe_return" -> instr (ICall CufSafeReturn)
    | "fb_call_user_func_safe"
    | "fb_call_user_func_array_safe" -> instr (ICall CufSafeArray)
    | _ -> empty
  in
  let flavor = match id with
    | "fb_call_user_func_safe"
    | "fb_call_user_func_array_safe" -> Flavor.Cell
    | _ -> Flavor.ReturnVal
  in
  gather [
    (* first arg is always emitted as cell *)
    emit_expr ~need_ref:false env (strip_ref arg);
    begin_instr;
    gather (List.mapi args (emit_call_user_func_arg env id));
    call_instr;
    end_instr;
  ], flavor

(* TODO: work out what HHVM does special here *)
and emit_name_string env e =
  emit_expr ~need_ref:false env e

and emit_special_function env pos id args uargs default =
  let nargs = List.length args + List.length uargs in
  let fq_id, _ =
    Hhbc_id.Function.elaborate_id_with_builtins (Emit_env.get_namespace env) (Pos.none, id) in
  (* Make sure that we do not treat a special function that is aliased as not
   * aliased *)
  let lower_fq_name =
    String.lowercase_ascii (Hhbc_id.Function.to_raw_string fq_id) in
  let hh_enabled = Emit_env.is_hh_syntax_enabled () in
  match lower_fq_name, args with
  | id, _ when id = SN.SpecialFunctions.echo ->
    let instrs = gather @@ List.mapi args begin fun i arg ->
         gather [
           emit_expr ~need_ref:false env arg;
           Emit_pos.emit_pos pos;
           instr (IOp Print);
           if i = nargs-1 then empty else instr_popc
         ] end in
    Some (instrs, Flavor.Cell)

  | "array_slice", [
    _, A.Call ((_, A.Id (_, "func_get_args")), _, [], []);
    (_, A.Int _ as count)
    ] when not (Hhbc_options.jit_enable_rename_function !Hhbc_options.compiler_options) ->
    let p = Pos.none in
    Some (emit_call env pos (p,
        A.Id (p, "\\__SystemLib\\func_slice_args")) [count] [])

  | "hh\\asm", [_, A.String (_, s)] ->
    Some (emit_inline_hhas s, Flavor.Cell)

  | id, _ when
    (optimize_cuf ()) && (is_call_user_func id (List.length args)) ->
    if List.length uargs != 0 then
    failwith "Using argument unpacking for a call_user_func is not supported";
    begin match args with
      | [] -> failwith "call_user_func - needs a name"
      | arg :: args ->
        Some (emit_call_user_func env id arg args)
    end

  | "hh\\invariant", e::rest when hh_enabled ->
    let l = Label.next_regular () in
    let p = Pos.none in
    let expr_id = p, A.Id (p, "\\hh\\invariant_violation") in
    Some (gather [
      (* Could use emit_jmpnz for better code *)
      emit_expr ~need_ref:false env e;
      instr_jmpnz l;
      emit_ignored_expr env (p, A.Call (expr_id, [], rest, uargs));
      Emit_fatal.emit_fatal_runtime p "invariant_violation";
      instr_label l;
      instr_null;
    ], Flavor.Cell)

  | "assert", _ ->
    let l0 = Label.next_regular () in
    let l1 = Label.next_regular () in
    Some (gather [
      instr_string "zend.assertions";
      instr_fcallbuiltin 1 1 "ini_get";
      instr_unboxr_nop;
      instr_int 0;
      instr_gt;
      instr_jmpz l0;
      fst @@ default ();
      instr_unboxr;
      instr_jmp l1;
      instr_label l0;
      instr_true;
      instr_label l1;
    ], Flavor.Cell)

  | ("class_exists" | "interface_exists" | "trait_exists" as id), arg1::_
    when nargs = 1 || nargs = 2 ->
    let class_kind =
      match id with
      | "class_exists" -> KClass
      | "interface_exists" -> KInterface
      | "trait_exists" -> KTrait
      | _ -> failwith "class_kind" in
    Some (gather [
      emit_name_string env arg1;
      instr (IOp CastString);
      if nargs = 1 then instr_true
      else gather [
        emit_expr ~need_ref:false env (List.nth_exn args 1);
        instr (IOp CastBool)
      ];
      instr (IMisc (OODeclExists class_kind))
    ], Flavor.Cell)

  | ("exit" | "die"), _ when nargs = 0 || nargs = 1 ->
    Some (emit_exit env (List.hd args), Flavor.Cell)

  | _ ->
    begin match args, istype_op lower_fq_name with
    | [(_, A.Lvar (_, arg_str as arg_id))], Some i
      when not (is_local_this env arg_str) ->
      Some (instr (IIsset (IsTypeL (get_local env arg_id, i))), Flavor.Cell)
    | [arg_expr], Some i ->
      Some (gather [
        emit_expr ~need_ref:false env arg_expr;
        instr (IIsset (IsTypeC i))
      ], Flavor.Cell)
    | _ ->
      begin match get_call_builtin_func_info lower_fq_name with
      | Some (nargs, i) when nargs = List.length args ->
        Some (
          gather [
          emit_exprs env args;
          instr i
        ], Flavor.Cell)
      | _ -> None
      end
    end

and get_inout_arg_positions args =
  List.filter_mapi args
    ~f:(fun i -> function
          | _, A.Callconv (A.Pinout, _) -> Some i
          | _ -> None)

and emit_call env pos (_, expr_ as expr) args uargs =
  (match expr_ with
    | A.Id (_, s) -> Emit_symbol_refs.add_function s
    | _ -> ());
  let nargs = List.length args + List.length uargs in
  let inout_arg_positions = get_inout_arg_positions args in
  let default () =
    let flavor = if List.length inout_arg_positions = 0 then
      Flavor.ReturnVal else Flavor.Cell in
    gather [
      emit_call_lhs
        env expr nargs (not (List.is_empty uargs)) inout_arg_positions;
      emit_args_and_call env args uargs;
    ], flavor in

  match expr_, args with
  | A.Id (_, id), _ ->
    let special_fn_opt = emit_special_function env pos id args uargs default in
    begin match special_fn_opt with
    | Some (instrs, flavor) -> instrs, flavor
    | None -> default ()
    end
  | _ -> default ()


(* Emit code for an expression that might leave a cell or reference on the
 * stack. Return which flavor it left.
 *)
and emit_flavored_expr env (pos, expr_ as expr) =
  match expr_ with
  | A.Call (e, _, args, uargs)
    when not (is_special_function env e args) ->
    let instrs, flavor = emit_call env pos e args uargs in
    Emit_pos.emit_pos_then pos instrs, flavor
  | A.Execution_operator es ->
    emit_execution_operator env es, Flavor.ReturnVal
  | _ ->
    let flavor =
      if binary_assignment_rhs_starts_with_ref expr
      then Flavor.Ref
      else Flavor.Cell
    in
    emit_expr ~need_ref:false env expr, flavor

and emit_final_member_op stack_index op mk =
  match op with
  | LValOp.Set -> instr (IFinal (SetM (stack_index, mk)))
  | LValOp.SetRef -> instr (IFinal (BindM (stack_index, mk)))
  | LValOp.SetOp op -> instr (IFinal (SetOpM (stack_index, op, mk)))
  | LValOp.IncDec op -> instr (IFinal (IncDecM (stack_index, op, mk)))
  | LValOp.Unset -> instr (IFinal (UnsetM (stack_index, mk)))

and emit_final_local_op op lid =
  match op with
  | LValOp.Set -> instr (IMutator (SetL lid))
  | LValOp.SetRef -> instr (IMutator (BindL lid))
  | LValOp.SetOp op -> instr (IMutator (SetOpL (lid, op)))
  | LValOp.IncDec op -> instr (IMutator (IncDecL (lid, op)))
  | LValOp.Unset -> instr (IMutator (UnsetL lid))

and emit_final_named_local_op op =
  match op with
  | LValOp.Set -> instr (IMutator SetN)
  | LValOp.SetRef -> instr (IMutator BindN)
  | LValOp.SetOp op -> instr (IMutator (SetOpN op))
  | LValOp.IncDec op -> instr (IMutator (IncDecN op))
  | LValOp.Unset -> instr (IMutator UnsetN)

and emit_final_global_op op =
  match op with
  | LValOp.Set -> instr (IMutator SetG)
  | LValOp.SetRef -> instr (IMutator BindG)
  | LValOp.SetOp op -> instr (IMutator (SetOpG op))
  | LValOp.IncDec op -> instr (IMutator (IncDecG op))
  | LValOp.Unset -> instr (IMutator UnsetG)

and emit_final_static_op cid prop op =
  match op with
  | LValOp.Set -> instr (IMutator (SetS 0))
  | LValOp.SetRef -> instr (IMutator (BindS 0))
  | LValOp.SetOp op -> instr (IMutator (SetOpS (op, 0)))
  | LValOp.IncDec op -> instr (IMutator (IncDecS (op, 0)))
  | LValOp.Unset ->
    let cid = text_of_expr cid in
    let id = text_of_expr (snd prop) in
    Emit_fatal.emit_fatal_runtime (fst id)
      ("Attempt to unset static property " ^ snd cid ^ "::" ^ snd id)

(* Given a local $local and a list of integer array indices i_1, ..., i_n,
 * generate code to extract the value of $local[i_n]...[i_1]:
 *   BaseL $local Warn
 *   Dim Warn EI:i_n ...
 *   Dim Warn EI:i_2
 *   QueryM 0 CGet EI:i_1
 *)
and emit_array_get_fixed local indices =
  gather (
    instr (IBase (BaseL (local, MemberOpMode.Warn))) ::
    List.rev_mapi indices (fun i ix ->
      let mk = MemberKey.EI (Int64.of_int ix) in
      if i = 0
      then instr (IFinal (QueryM (0, QueryOp.CGet, mk)))
      else instr (IBase (Dim (MemberOpMode.Warn, mk))))
      )

and can_use_as_rhs_in_list_assignment expr =
  match expr with
  | A.Lvar _
  | A.Dollar _
  | A.Array_get _
  | A.Obj_get _
  | A.Class_get _
  | A.Call _
  | A.New _
  | A.Expr_list _
  | A.Yield _
  | A.NullCoalesce _
  | A.Cast _
  | A.Eif _
  | A.Array _
  | A.Varray _
  | A.Darray _
  | A.Collection _
  | A.Clone _
  | A.Unop _
  | A.Await _ -> true
  | A.Pipe (_, (_, r))
  | A.Binop ((A.Eq None), (_, A.List _), (_, r)) ->
    can_use_as_rhs_in_list_assignment r
  | A.Binop (A.Plus, _, _)
  | A.Binop (A.Eq _, _, _) -> true
  | _ -> false


(* Generate code for each lvalue assignment in a list destructuring expression.
 * Lvalues are assigned right-to-left, regardless of the nesting structure. So
 *     list($a, list($b, $c)) = $d
 * and list(list($a, $b), $c) = $d
 * will both assign to $c, $b and $a in that order.
 * Returns a pair of instructions:
 * 1. initialization part of the left hand side
 * 2. assignment
 * this is necessary to handle cases like:
 * list($a[$f()]) = b();
 * here f() should be invoked before b()
 *)
 and emit_lval_op_list env local indices expr =
  match snd expr with
  | A.List exprs ->
    let lhs_instrs, set_instrs =
      List.mapi exprs (fun i expr -> emit_lval_op_list env local (i::indices) expr)
      |> List.unzip in
    gather lhs_instrs,
    gather (List.rev set_instrs)
  | A.Omitted -> empty, empty
  | _ ->
    (* Generate code to access the element from the array *)
    let access_instrs =
      match local with
      | Some local -> emit_array_get_fixed local indices
      | None -> instr_null
    in
    (* Generate code to assign to the lvalue *)
    (* Return pair: side effects to initialize lhs + assignment *)
    let lhs_instrs, rhs_instrs, set_op =
      emit_lval_op_nonlist_steps env LValOp.Set expr access_instrs 1 in
    lhs_instrs,
    gather [
      rhs_instrs;
      set_op;
      instr_popc
    ]

and expr_starts_with_ref = function
  | _, A.Unop (A.Uref, _) -> true
  | _ -> false

and binary_assignment_rhs_starts_with_ref = function
  | _, A.Binop (A.Eq None, _, e) when expr_starts_with_ref e -> true
  | _ -> false

and emit_expr_and_unbox_if_necessary ~need_ref env e =
  let unboxing_instr =
    if binary_assignment_rhs_starts_with_ref e
    then instr_unbox
    else empty
  in
  gather [emit_expr ~need_ref env e; unboxing_instr]

(* Emit code for an l-value operation *)
and emit_lval_op env pos op expr1 opt_expr2 =
  let op =
    match op, opt_expr2 with
    | LValOp.Set, Some e when expr_starts_with_ref e -> LValOp.SetRef
    | _ -> op
  in
  match op, expr1, opt_expr2 with
    (* Special case for list destructuring, only on assignment *)
    | LValOp.Set, (_, A.List l), Some expr2 ->
      let has_elements =
        List.exists l ~f: (function
          | _, A.Omitted -> false
          | _ -> true)
      in
      if has_elements then
        stash_in_local_with_prefix ~leave_on_stack:true env expr2
        begin fun local _break_label ->
          let local =
            if can_use_as_rhs_in_list_assignment (snd expr2) then
              Some local
            else
              None
          in
          emit_lval_op_list env local [] expr1
        end
      else
        Local.scope @@ fun () ->
          let local = Local.get_unnamed_local () in
          gather [
            emit_expr ~need_ref:false env expr2;
            instr_setl local;
            instr_popc;
            instr_pushl local;
          ]
    | _ ->
      Local.scope @@ fun () ->
        let rhs_instrs, rhs_stack_size =
          match opt_expr2 with
          | None -> empty, 0
          | Some (_, A.Yield af) ->
            let temp = Local.get_unnamed_local () in
            gather [
              emit_yield env af;
              instr_setl temp;
              instr_popc;
              instr_pushl temp;
            ], 1
          | Some (pos, A.Unop (A.Uref, (_, A.Obj_get (_, _, A.OG_nullsafe)
                                    | _, A.Array_get ((_,
                                      A.Obj_get (_, _, A.OG_nullsafe)), _)))) ->
            Emit_fatal.raise_fatal_runtime
              pos "?-> is not allowed in write context"
          | Some e -> emit_expr_and_unbox_if_necessary ~need_ref:false env e, 1
        in
        emit_lval_op_nonlist env pos op expr1 rhs_instrs rhs_stack_size

and emit_lval_op_nonlist env pos op e rhs_instrs rhs_stack_size =
  let (lhs, rhs, setop) =
    emit_lval_op_nonlist_steps env op e rhs_instrs rhs_stack_size
  in
  gather [
    lhs;
    rhs;
    Emit_pos.emit_pos pos;
    setop;
  ]

and emit_lval_op_nonlist_steps env op (pos, expr_) rhs_instrs rhs_stack_size =
  let handle_dollar e final_op =
    match e with
      _, A.Lvar id ->
      let instruction =
        let local = (get_local env id) in
        match op with
        | LValOp.Unset | LValOp.IncDec _ -> instr_cgetl local
        | _ -> instr_cgetl2 local
      in
      empty,
      rhs_instrs,
      gather [
        instruction;
        final_op op
      ]
    | _ ->
      let instrs = emit_expr ~need_ref:false env e in
      instrs,
      rhs_instrs,
      final_op op
  in
  match expr_ with
  | A.Lvar (_, id) when SN.Superglobals.is_superglobal id ->
    instr_string @@ SU.Locals.strip_dollar id,
    rhs_instrs,
    emit_final_global_op op

  | A.Lvar ((_, str) as id) when is_local_this env str && is_incdec op ->
    emit_local ~notice:Notice ~need_ref:false env id,
    rhs_instrs,
    empty

  | A.Lvar id when not (is_local_this env (snd id)) || op = LValOp.Unset ->
    empty,
    rhs_instrs,
    emit_final_local_op op (get_local env id)

  | A.Dollar e ->
    handle_dollar e emit_final_named_local_op

  | A.Array_get ((_, A.Lvar (_, x)), Some e) when x = SN.Superglobals.globals ->
    let final_global_op_instrs = emit_final_global_op op in
    if rhs_stack_size = 0
    then
      emit_expr ~need_ref:false env e,
      empty,
      final_global_op_instrs
    else
      let index_instrs, under_top = emit_first_expr env e in
      if under_top
      then
        empty,
        gather [
          rhs_instrs;
          index_instrs
        ],
        final_global_op_instrs
      else
        index_instrs,
        rhs_instrs,
        final_global_op_instrs

  | A.Array_get (base_expr, opt_elem_expr) ->
    let mode =
      match op with
      | LValOp.Unset -> MemberOpMode.Unset
      | _ -> MemberOpMode.Define in
    let elem_expr_instrs, elem_stack_size = emit_elem_instrs env opt_elem_expr in
    let base_offset = elem_stack_size + rhs_stack_size in
    let base_expr_instrs_begin,
        base_expr_instrs_end,
        base_setup_instrs,
        base_stack_size =
      emit_base
        ~notice:Notice ~is_object:false
        env mode base_offset None base_expr
    in
    let mk = get_elem_member_key env rhs_stack_size opt_elem_expr in
    let total_stack_size = elem_stack_size + base_stack_size in
    let final_instr =
      Emit_pos.emit_pos_then pos @@
      emit_final_member_op total_stack_size op mk in
    gather [
      base_expr_instrs_begin;
      elem_expr_instrs;
      base_expr_instrs_end;
    ],
    rhs_instrs,
    gather [
      base_setup_instrs;
      final_instr
    ]

  | A.Obj_get (e1, e2, null_flavor) ->
    if null_flavor = A.OG_nullsafe then
     Emit_fatal.raise_fatal_parse pos "?-> is not allowed in write context";
    let mode =
      match op with
      | LValOp.Unset -> MemberOpMode.Unset
      | _ -> MemberOpMode.Define in
    let mk, prop_expr_instrs, prop_stack_size =
      emit_prop_expr env null_flavor rhs_stack_size e2 in
    let base_offset = prop_stack_size + rhs_stack_size in
    let base_expr_instrs_begin,
        base_expr_instrs_end,
        base_setup_instrs,
        base_stack_size =
      emit_base
        ~notice:Notice ~is_object:true
        env mode base_offset None e1
    in
    let total_stack_size = prop_stack_size + base_stack_size in
    let final_instr =
      Emit_pos.emit_pos_then pos @@
      emit_final_member_op total_stack_size op mk in
    gather [
      base_expr_instrs_begin;
      prop_expr_instrs;
      base_expr_instrs_end;
    ],
    rhs_instrs,
    gather [
      base_setup_instrs;
      final_instr
    ]

  | A.Class_get (cid, prop) ->
    let cexpr = expr_to_class_expr ~resolve_self:false
      (Emit_env.get_scope env) cid in
    begin match snd prop with
    | A.Dollar (_, A.Lvar _ as e) ->
      let final_instr = emit_final_static_op (snd cid) prop op in
      let instrs, under_top = emit_first_expr env e in
      if under_top
      then
        emit_load_class_ref env cexpr,
        rhs_instrs,
        gather [instrs; final_instr]
      else
        gather [instrs; emit_load_class_ref env cexpr],
        rhs_instrs,
        final_instr
    | _ ->
      let final_instr =
        Emit_pos.emit_pos_then pos @@
        emit_final_static_op (snd cid) prop op in
      emit_class_expr env cexpr prop,
      rhs_instrs,
      final_instr
    end

  | A.Unop (uop, e) ->
    empty,
    rhs_instrs,
    gather [
      emit_lval_op_nonlist env pos op e empty rhs_stack_size;
      from_unop uop;
    ]

  | _ ->
    Emit_fatal.raise_fatal_parse pos "Can't use return value in write context"

and from_unop op =
  let ints_overflow_to_ints =
    Hhbc_options.ints_overflow_to_ints !Hhbc_options.compiler_options
  in
  match op with
  | A.Utild -> instr (IOp BitNot)
  | A.Unot -> instr (IOp Not)
  | A.Uplus -> instr (IOp (if ints_overflow_to_ints then Add else AddO))
  | A.Uminus -> instr (IOp (if ints_overflow_to_ints then Sub else SubO))
  | A.Uincr | A.Udecr | A.Upincr | A.Updecr | A.Uref | A.Usilence ->
    emit_nyi "unop - probably does not need translation"

and emit_expr_as_ref env e = emit_expr ~need_ref:true env e

and emit_unop ~need_ref env pos op e =
  let unop_instr = from_unop op in
  match op with
  | A.Utild ->
    emit_box_if_necessary need_ref @@ gather [
      emit_expr ~need_ref:false env e; unop_instr
    ]
  | A.Unot ->
    emit_box_if_necessary need_ref @@ gather [
      emit_expr ~need_ref:false env e; unop_instr
    ]
  | A.Uplus ->
    emit_box_if_necessary need_ref @@ gather
    [instr (ILitConst (Int (Int64.zero)));
    emit_expr ~need_ref:false env e;
    unop_instr]
  | A.Uminus ->
    emit_box_if_necessary need_ref @@ gather
    [instr (ILitConst (Int (Int64.zero)));
    emit_expr ~need_ref:false env e;
    unop_instr]
  | A.Uincr | A.Udecr | A.Upincr | A.Updecr ->
    begin match unop_to_incdec_op op with
    | None -> emit_nyi "incdec"
    | Some incdec_op ->
      let instr = emit_lval_op env pos (LValOp.IncDec incdec_op) e None in
      emit_box_if_necessary need_ref instr
    end
  | A.Uref -> emit_expr_as_ref env e
  | A.Usilence ->
    Local.scope @@ fun () ->
      let fault_label = Label.next_fault () in
      let temp_local = Local.get_unnamed_local () in
      let cleanup = instr_silence_end temp_local in
      let body = gather [emit_expr ~need_ref:false env e; cleanup] in
      let fault = gather [cleanup; Emit_pos.emit_pos pos; instr_unwind] in
      gather [
        instr_silence_start temp_local;
        instr_try_fault fault_label body fault
      ]

and emit_exprs env exprs =
  gather (List.map exprs (emit_expr ~need_ref:false env))

(* allows to create a block of code that will
- get a fresh temporary local
- be wrapped in a try/fault where fault will clean temporary from the previous
  bulletpoint*)
and with_temp_local temp f =
  let _, block =
    with_temp_local_with_prefix temp (fun temp label -> empty, f temp label) in
  block

(* similar to with_temp_local with addition that
  function 'f' that creates result block of code can generate an
  additional prefix instruction sequence that should be
  executed before the result block *)
and with_temp_local_with_prefix temp f =
  let break_label = Label.next_regular () in
  let prefix, block = f temp break_label in
  if is_empty block then prefix, block
  else
    let fault_label = Label.next_fault () in
    prefix,
    gather [
      instr_try_fault
        fault_label
        (* try block *)
        block
        (* fault block *)
        (gather [
          instr_unsetl temp;
          instr_unwind ]);
      instr_label break_label;
    ]

(* Similar to stash_in_local with addition that function that
   creates a block of code can yield a prefix instrution
  that will be executed as the first instruction in the result instruction set *)
and stash_in_local_with_prefix ?(always_stash=false) ?(leave_on_stack=false)
                   ?(always_stash_this=false) env e f =
  match e with
  | (_, A.Lvar id) when not always_stash
    && not (is_local_this env (snd id) &&
    ((Emit_env.get_needs_local_this env) || always_stash_this)) ->
    let break_label = Label.next_regular () in
    let prefix_instr, result_instr =
      f (get_local env id) break_label in
    gather [
      prefix_instr;
      result_instr;
      instr_label break_label;
      if leave_on_stack then instr_cgetl (get_local env id) else empty;
    ]
  | _ ->
    let generate_value =
      Local.scope @@ fun () -> emit_expr ~need_ref:false env e in
    Local.scope @@ fun () ->
      let temp = Local.get_unnamed_local () in
      let prefix_instr, result_instr =
        with_temp_local_with_prefix temp f in
      gather [
        prefix_instr;
        generate_value;
        instr_setl temp;
        instr_popc;
        result_instr;
        if leave_on_stack then instr_pushl temp else instr_unsetl temp
      ]
(* Generate code to evaluate `e`, and, if necessary, store its value in a
 * temporary local `temp` (unless it is itself a local). Then use `f` to
 * generate code that uses this local and branches or drops through to
 * `break_label`:
 *    temp := e
 *    <code generated by `f temp break_label`>
 *  break_label:
 *    push `temp` on stack if `leave_on_stack` is true.
 *)
and stash_in_local ?(always_stash=false) ?(leave_on_stack=false)
                   ?(always_stash_this=false) env e f =
  stash_in_local_with_prefix ~always_stash ~leave_on_stack ~always_stash_this
    env e (fun temp label -> empty, f temp label)
