" Wolang highlighting for Vim and Neovim.
"
" Regular expressions, which is what a syntax file is: the real parser is
" in the compiler, and this exists so that a file looks right before the
" language server has answered.

if exists("b:current_syntax")
  finish
endif

syn keyword wolangControl if else match loop while for foreach in return
syn keyword wolangControl break continue defer spawn wait on enter exit
syn keyword wolangControl state always
syn keyword wolangDeclare fun task class struct enum ability component tag
syn keyword wolangDeclare machine system type var const extend extern
syn keyword wolangDeclare signal effect dimension unit deinit derived
syn keyword wolangDeclare virtual override offer priv need
syn keyword wolangOther as mut uses where unsafe arena comptime self error
syn keyword wolangOther with without before after aos soa
syn keyword wolangBool true false null
syn keyword wolangType int float bool text str Text void never
syn keyword wolangType s8 s16 s32 s64 u8 u16 u32 u64 f32 f64

syn match wolangAttribute "@\w\+"
syn match wolangNumber "\<\d[0-9_]*\(\.\d[0-9_]*\)\?\([eE][-+]\?\d\+\)\?\w*\>"
syn match wolangNumber "\<0x[0-9A-Fa-f_]\+\>"
syn match wolangNumber "\<0b[01_]\+\>"
syn match wolangTypeName "\<\u\w*\>"
syn region wolangString start=/"/ skip=/\\./ end=/"/ contains=wolangEscape,wolangInterp
syn match wolangEscape /\\./ contained
syn region wolangInterp start=/{/ end=/}/ contained contains=wolangNumber
syn region wolangComment start="//" end="$" contains=@Spell
syn region wolangDoc start="///" end="$" contains=@Spell
syn region wolangComment start="/\*" end="\*/" contains=@Spell

hi def link wolangControl Statement
hi def link wolangDeclare Keyword
hi def link wolangOther Keyword
hi def link wolangBool Boolean
hi def link wolangType Type
hi def link wolangTypeName Type
hi def link wolangAttribute PreProc
hi def link wolangNumber Number
hi def link wolangString String
hi def link wolangEscape SpecialChar
hi def link wolangInterp Special
hi def link wolangComment Comment
hi def link wolangDoc SpecialComment

let b:current_syntax = "wolang"
