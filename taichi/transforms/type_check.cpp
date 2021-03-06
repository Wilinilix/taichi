// Type checking

#include "../ir.h"
#include <taichi/lang.h>

TLANG_NAMESPACE_BEGIN

// "Type" here does not include vector width
// Var lookup and Type inference
class TypeCheck : public IRVisitor {
 public:
  TypeCheck() {
    allow_undefined_visitor = true;
  }

  static void mark_as_if_const(Stmt *stmt, VectorType t) {
    if (stmt->is<ConstStmt>()) {
      stmt->ret_type = t;
    }
  }

  void visit(AllocaStmt *stmt) {
    // Do nothing.
    // Alloca type is determined by first (compile-time) LocalStore
  }

  void visit(IfStmt *if_stmt) {
    if (if_stmt->true_statements)
      if_stmt->true_statements->accept(this);
    if (if_stmt->false_statements) {
      if_stmt->false_statements->accept(this);
    }
  }

  void visit(Block *stmt_list) {
    std::vector<Stmt *> stmts;
    // Make a copy since type casts may be inserted for type promotion
    for (auto &stmt : stmt_list->statements) {
      stmts.push_back(stmt.get());
      // stmt->accept(this);
    }
    for (auto stmt : stmts)
      stmt->accept(this);
  }

  void visit(AtomicOpStmt *stmt) {
    TC_ASSERT(stmt->width() == 1);
    if (stmt->val->ret_type.data_type != stmt->dest->ret_type.data_type) {
      TC_WARN("Atomic add ({} to {}) may lose precision.",
              data_type_name(stmt->val->ret_type.data_type),
              data_type_name(stmt->dest->ret_type.data_type));
      stmt->val = insert_type_cast_before(stmt, stmt->val,
                                          stmt->dest->ret_type.data_type);
    }
  }

  void visit(LocalLoadStmt *stmt) {
    TC_ASSERT(stmt->width() == 1);
    auto lookup = stmt->ptr[0].var->ret_type;
    stmt->ret_type = lookup;
  }

  void visit(LocalStoreStmt *stmt) {
    if (stmt->ptr->ret_type.data_type == DataType::unknown) {
      // Infer data type for alloca
      stmt->ptr->ret_type = stmt->data->ret_type;
    }
    auto ret_type = promoted_type(stmt->ptr->ret_type.data_type,
                                  stmt->data->ret_type.data_type);
    if (ret_type != stmt->data->ret_type.data_type) {
      stmt->data = insert_type_cast_before(stmt, stmt->data,
                                           stmt->ptr->ret_type.data_type);
    }
    if (stmt->ptr->ret_type.data_type != ret_type) {
      TC_WARN(
          "Local store may lose precision (target = {}, value = {}, "
          "stmt_id = {}) at",
          stmt->ptr->ret_data_type_name(), stmt->data->ret_data_type_name(),
          stmt->id);
      fmt::print(stmt->tb);
    }
    stmt->ret_type = stmt->ptr->ret_type;
  }

  void visit(GlobalLoadStmt *stmt) {
    stmt->ret_type = stmt->ptr->ret_type;
  }

  void visit(SNodeOpStmt *stmt) {
    stmt->ret_type = VectorType(1, DataType::i32);
  }

  void visit(GlobalPtrStmt *stmt) {
    if (stmt->snodes)
      stmt->ret_type.data_type = stmt->snodes[0]->dt;
    else
      TC_WARN("Type inference failed: snode is nullptr.");
    for (int l = 0; l < stmt->snodes.size(); l++) {
      if (stmt->snodes[l]->parent->num_active_indices != 0 &&
          stmt->snodes[l]->parent->num_active_indices != stmt->indices.size()) {
        TC_ERROR("{} has {} indices. Indexed with {}.",
                 stmt->snodes[l]->parent->node_type_name,
                 stmt->snodes[l]->parent->num_active_indices,
                 stmt->indices.size());
      }
    }
    for (int i = 0; i < stmt->indices.size(); i++) {
      TC_ASSERT_INFO(
          is_integral(stmt->indices[i]->ret_type.data_type),
          "Taichi tensors must be accessed with integral indices (e.g., "
          "i32/i64). It seems that you have used a float point number as "
          "an index. You can cast that to an integer using int(). Also note "
          "that ti.floor(ti.f32) returns f32.");
      TC_ASSERT(stmt->indices[i]->ret_type.width == stmt->snodes.size());
    }
  }

  void visit(GlobalStoreStmt *stmt) {
    auto promoted = promoted_type(stmt->ptr->ret_type.data_type,
                                  stmt->data->ret_type.data_type);
    auto input_type = stmt->data->ret_data_type_name();
    if (stmt->ptr->ret_type.data_type != stmt->data->ret_type.data_type) {
      stmt->data = insert_type_cast_before(stmt, stmt->data,
                                           stmt->ptr->ret_type.data_type);
    }
    if (stmt->ptr->ret_type.data_type != promoted) {
      TC_WARN("Global store may lose precision: {} <- {}, at",
              stmt->ptr->ret_data_type_name(), input_type, stmt->tb);
    }
    stmt->ret_type = stmt->ptr->ret_type;
  }

  void visit(RangeForStmt *stmt) {
    /*
    TC_ASSERT(block->local_variables.find(stmt->loop_var) ==
              block->local_variables.end());
              */
    mark_as_if_const(stmt->begin, VectorType(1, DataType::i32));
    mark_as_if_const(stmt->end, VectorType(1, DataType::i32));
    /*
    block->local_variables.insert(
        std::make_pair(stmt->loop_var, VectorType(1, DataType::i32)));
    */
    stmt->body->accept(this);
  }

  void visit(StructForStmt *stmt) {
    stmt->body->accept(this);
  }

  void visit(WhileStmt *stmt) {
    stmt->body->accept(this);
  }

  void visit(UnaryOpStmt *stmt) {
    stmt->ret_type = stmt->operand->ret_type;
    if (stmt->op_type == UnaryOpType::cast) {
      stmt->ret_type.data_type = stmt->cast_type;
    }
    if (is_trigonometric(stmt->op_type) &&
        !is_real(stmt->operand->ret_type.data_type)) {
      TC_ERROR("Trigonometric operator takes real inputs only. At {}",
               stmt->tb);
    }
    if ((stmt->op_type == UnaryOpType::floor ||
         stmt->op_type == UnaryOpType::ceil) &&
        !is_real(stmt->operand->ret_type.data_type)) {
      TC_ERROR("floor/ceil takes real inputs only. At {}", stmt->tb);
    }
  }

  Stmt *insert_type_cast_before(Stmt *anchor,
                                Stmt *input,
                                DataType output_type) {
    auto &&cast_stmt = Stmt::make_typed<UnaryOpStmt>(UnaryOpType::cast, input);
    cast_stmt->cast_type = output_type;
    cast_stmt->cast_by_value = true;
    cast_stmt->accept(this);
    auto stmt = cast_stmt.get();
    anchor->insert_before_me(std::move(cast_stmt));
    return stmt;
  }

  Stmt *insert_type_cast_after(Stmt *anchor,
                               Stmt *input,
                               DataType output_type) {
    auto &&cast_stmt = Stmt::make_typed<UnaryOpStmt>(UnaryOpType::cast, input);
    cast_stmt->cast_type = output_type;
    cast_stmt->cast_by_value = true;
    cast_stmt->accept(this);
    auto stmt = cast_stmt.get();
    anchor->insert_after_me(std::move(cast_stmt));
    return stmt;
  }

  void cast(Stmt *&val, DataType dt) {
    auto cast_stmt = insert_type_cast_after(val, val, dt);
    val = cast_stmt;
  }

  void visit(BinaryOpStmt *stmt) {
    auto error = [&](std::string comment = "") {
      if (comment == "") {
        TC_WARN("Error: type mismatch (left = {}, right = {}, stmt_id = {}) at",
                stmt->lhs->ret_data_type_name(),
                stmt->rhs->ret_data_type_name(), stmt->id);
      } else {
        TC_WARN(comment + " at");
      }
      fmt::print(stmt->tb);
      TC_WARN("Compilation stopped due to type mismatch.");
      exit(-1);
    };
    if (stmt->lhs->ret_type.data_type == DataType::unknown &&
        stmt->rhs->ret_type.data_type == DataType::unknown)
      error();

    // lower truediv and floordiv into div

    if (stmt->op_type == BinaryOpType::floordiv) {
      auto default_ip = get_current_program().config.default_ip;
      if (!is_integral(stmt->lhs->ret_type.data_type)) {
        cast(stmt->lhs, default_ip);
      }
      if (!is_integral(stmt->rhs->ret_type.data_type)) {
        cast(stmt->rhs, default_ip);
      }
      stmt->op_type = BinaryOpType::div;
    }

    if (stmt->op_type == BinaryOpType::truediv) {
      auto default_fp = get_current_program().config.default_fp;
      if (!is_real(stmt->lhs->ret_type.data_type)) {
        cast(stmt->lhs, default_fp);
      }
      if (!is_real(stmt->rhs->ret_type.data_type)) {
        cast(stmt->rhs, default_fp);
      }
      stmt->op_type = BinaryOpType::div;
    }

    if (stmt->lhs->ret_type.data_type != stmt->rhs->ret_type.data_type) {
      auto ret_type = promoted_type(stmt->lhs->ret_type.data_type,
                                    stmt->rhs->ret_type.data_type);
      if (ret_type != stmt->lhs->ret_type.data_type) {
        // promote rhs
        auto cast_stmt = insert_type_cast_before(stmt, stmt->lhs, ret_type);
        stmt->lhs = cast_stmt;
      }
      if (ret_type != stmt->rhs->ret_type.data_type) {
        // promote rhs
        auto cast_stmt = insert_type_cast_before(stmt, stmt->rhs, ret_type);
        stmt->rhs = cast_stmt;
      }
    }
    bool matching = true;
    matching =
        matching && (stmt->lhs->ret_type.width == stmt->rhs->ret_type.width);
    matching = matching && (stmt->lhs->ret_type.data_type != DataType::unknown);
    matching = matching && (stmt->rhs->ret_type.data_type != DataType::unknown);
    matching = matching && (stmt->lhs->ret_type == stmt->rhs->ret_type);
    if (!matching) {
      error();
    }
    if (binary_is_bitwise(stmt->op_type)) {
      if (!is_integral(stmt->lhs->ret_type.data_type)) {
        error("Error: bitwise operations can only apply to integral types.");
      }
    }
    if (is_comparison(stmt->op_type)) {
      stmt->ret_type = VectorType(stmt->lhs->ret_type.width, DataType::i32);
    } else {
      stmt->ret_type = stmt->lhs->ret_type;
    }
  }

  void visit(TernaryOpStmt *stmt) {
    if (stmt->op_type == TernaryOpType::select) {
      auto ret_type = promoted_type(stmt->op2->ret_type.data_type,
                                    stmt->op3->ret_type.data_type);
      TC_ASSERT(stmt->op1->ret_type.data_type == DataType::i32)
      TC_ASSERT(stmt->op1->ret_type.width == stmt->op2->ret_type.width);
      TC_ASSERT(stmt->op2->ret_type.width == stmt->op3->ret_type.width);
      if (ret_type != stmt->op2->ret_type.data_type) {
        auto cast_stmt = insert_type_cast_before(stmt, stmt->op2, ret_type);
        stmt->op2 = cast_stmt;
      }
      if (ret_type != stmt->op3->ret_type.data_type) {
        auto cast_stmt = insert_type_cast_before(stmt, stmt->op3, ret_type);
        stmt->op3 = cast_stmt;
      }
      stmt->ret_type = VectorType(stmt->op1->width(), ret_type);
    } else {
      TC_NOT_IMPLEMENTED
    }
  }

  void visit(ElementShuffleStmt *stmt) {
    TC_ASSERT(stmt->elements.size() != 0);
    stmt->element_type() = stmt->elements[0].stmt->element_type();
  }

  void visit(RangeAssumptionStmt *stmt) {
    TC_ASSERT(stmt->input->ret_type == stmt->base->ret_type);
    stmt->ret_type = stmt->input->ret_type;
  }

  void visit(ArgLoadStmt *stmt) {
    auto &args = get_current_program().get_current_kernel().args;
    TC_ASSERT(0 <= stmt->arg_id && stmt->arg_id < args.size());
    TC_ASSERT(!args[stmt->arg_id].is_return_value);
    stmt->ret_type = VectorType(1, args[stmt->arg_id].dt);
  }

  void visit(ArgStoreStmt *stmt) {
    auto &args = get_current_program().get_current_kernel().args;
    TC_ASSERT(0 <= stmt->arg_id && stmt->arg_id < args.size());
    auto arg = args[stmt->arg_id];
    auto arg_type = arg.dt;
    TC_ASSERT(arg.is_return_value);
    TC_ASSERT(stmt->val->ret_type.data_type == arg_type);
    stmt->ret_type = VectorType(1, arg_type);
  }

  void visit(ExternalPtrStmt *stmt) {
    stmt->ret_type = VectorType(stmt->base_ptrs.size(),
                                stmt->base_ptrs[0]->ret_type.data_type);
  }

  void visit(LoopIndexStmt *stmt) {
    stmt->ret_type = VectorType(1, DataType::i32);
  }

  void visit(GetChStmt *stmt) {
    stmt->ret_type = VectorType(1, stmt->output_snode->dt);
  }

  void visit(OffloadedStmt *stmt) {
    if (stmt->body)
      stmt->body->accept(this);
  }

  static void run(IRNode *node) {
    TypeCheck inst;
    node->accept(&inst);
  }
};

namespace irpass {

void typecheck(IRNode *root) {
  return TypeCheck::run(root);
}

}  // namespace irpass

TLANG_NAMESPACE_END
