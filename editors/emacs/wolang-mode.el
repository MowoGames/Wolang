;;; wolang-mode.el --- Major mode for Wolang -*- lexical-binding: t; -*-

;; A major mode and an Eglot server, which is all an editor needs to be:
;; `wolang lsp` answers everything -- diagnostics, hover, go to
;; definition, symbols, formatting, completion -- and this file is the
;; part that says `.wo` is Wolang and how it should look while typing.

;;; Code:

(defvar wolang-keywords
  '("fun" "task" "class" "struct" "enum" "ability" "component" "tag"
    "machine" "system" "type" "var" "const" "extend" "extern" "signal"
    "effect" "dimension" "unit" "deinit" "derived" "weak" "virtual" "override"
    "offer" "priv" "need" "if" "else" "match" "loop" "while" "for"
    "foreach" "in" "return" "break" "continue" "defer" "spawn" "wait"
    "on" "enter" "exit" "state" "always" "as" "mut" "uses" "where"
    "unsafe" "arena" "comptime" "self" "error" "with" "without" "before"
    "after" "aos" "soa")
  "Keywords of Wolang.")

(defvar wolang-types
  '("int" "float" "bool" "text" "str" "Text" "void" "never" "s8" "s16"
    "s32" "s64" "u8" "u16" "u32" "u64" "f32" "f64")
  "Built-in types of Wolang.")

(defvar wolang-font-lock-keywords
  `((,(concat "\\_<" (regexp-opt wolang-keywords) "\\_>")
     . font-lock-keyword-face)
    (,(concat "\\_<" (regexp-opt wolang-types) "\\_>")
     . font-lock-type-face)
    ("\\_<\\(true\\|false\\|null\\|ptrnull\\)\\_>" . font-lock-constant-face)
    ("@[A-Za-z_][A-Za-z0-9_]*" . font-lock-preprocessor-face)
    ("\\_<[A-Z][A-Za-z0-9_]*\\_>" . font-lock-type-face)
    ("\\_<\\([a-zA-Z_][A-Za-z0-9_]*\\)\\s-*(" 1 font-lock-function-name-face))
  "Highlighting for `wolang-mode'.")

(defvar wolang-mode-syntax-table
  (let ((table (make-syntax-table)))
    (modify-syntax-entry ?_ "w" table)
    (modify-syntax-entry ?/ ". 124" table)
    (modify-syntax-entry ?* ". 23b" table)
    (modify-syntax-entry ?\n ">" table)
    (modify-syntax-entry ?\" "\"" table)
    table)
  "Syntax table for `wolang-mode'.")

;;;###autoload
(define-derived-mode wolang-mode prog-mode "Wolang"
  "Major mode for editing Wolang."
  :syntax-table wolang-mode-syntax-table
  (setq-local font-lock-defaults '(wolang-font-lock-keywords))
  (setq-local comment-start "// ")
  (setq-local comment-end "")
  (setq-local indent-tabs-mode nil)
  (setq-local tab-width 2)
  (setq-local fill-column 80))

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.wo\\'" . wolang-mode))

;;;###autoload
(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs
               '(wolang-mode . ("wolang" "lsp"))))

(provide 'wolang-mode)
;;; wolang-mode.el ends here
