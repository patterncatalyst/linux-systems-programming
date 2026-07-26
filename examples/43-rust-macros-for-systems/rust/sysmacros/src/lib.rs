//! `sysmacros` — the proc-macro crate backing chapter 43's two techniques:
//!
//! - `#[derive(SysError)]`: a miniature `thiserror`. Each enum variant carries
//!   `#[error("...")]`; this derive generates `Display` (interpolating the
//!   variant's own fields via edition-2024 inline format-arg capture) and a
//!   blanket `std::error::Error` impl.
//! - `#[instrument]`: an attribute macro that wraps a free function to print
//!   an entry line (name + args), time the body with `Instant`, print an exit
//!   line (return value + elapsed microseconds), and return the value
//!   unchanged.
//!
//! Malformed input (a `#[derive(SysError)]` on a non-enum, or a variant
//! missing `#[error(...)]`) is reported as a real `compile_error!` via
//! `syn::Error::to_compile_error()` — never a panic.

use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use quote::{format_ident, quote};
use syn::{parse_macro_input, Data, DeriveInput, Fields, ItemFn, LitStr};

#[proc_macro_derive(SysError, attributes(error))]
pub fn derive_sys_error(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    match expand_sys_error(&input) {
        Ok(tokens) => tokens.into(),
        Err(err) => err.to_compile_error().into(),
    }
}

fn expand_sys_error(input: &DeriveInput) -> syn::Result<TokenStream2> {
    let name = &input.ident;

    let data_enum = match &input.data {
        Data::Enum(data_enum) => data_enum,
        _ => {
            return Err(syn::Error::new_spanned(
                input,
                "SysError can only be derived for enums",
            ))
        }
    };

    let mut arms = Vec::with_capacity(data_enum.variants.len());
    for variant in &data_enum.variants {
        let vident = &variant.ident;

        let error_attr = variant
            .attrs
            .iter()
            .find(|attr| attr.path().is_ident("error"))
            .ok_or_else(|| {
                syn::Error::new_spanned(
                    variant,
                    format!("variant `{vident}` is missing #[error(\"...\")]"),
                )
            })?;
        let message: LitStr = error_attr.parse_args()?;

        let arm = match &variant.fields {
            Fields::Named(fields) => {
                let idents: Vec<_> = fields
                    .named
                    .iter()
                    .map(|f| f.ident.clone().expect("named field always has an ident"))
                    .collect();
                // Edition-2024 inline format-arg capture: `path` and `errno`
                // in the literal resolve directly to the bindings destructured
                // in this match arm's pattern.
                quote! {
                    Self::#vident { #(#idents),* } => write!(f, #message),
                }
            }
            Fields::Unnamed(fields) => {
                let idents: Vec<_> = (0..fields.unnamed.len())
                    .map(|i| format_ident!("f{}", i))
                    .collect();
                quote! {
                    Self::#vident(#(#idents),*) => write!(f, #message, #(#idents),*),
                }
            }
            Fields::Unit => {
                quote! {
                    Self::#vident => write!(f, #message),
                }
            }
        };
        arms.push(arm);
    }

    Ok(quote! {
        impl ::std::fmt::Display for #name {
            fn fmt(&self, f: &mut ::std::fmt::Formatter<'_>) -> ::std::fmt::Result {
                match self {
                    #(#arms)*
                }
            }
        }

        impl ::std::error::Error for #name {}
    })
}

#[proc_macro_attribute]
pub fn instrument(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let func = parse_macro_input!(item as ItemFn);
    match expand_instrument(func) {
        Ok(tokens) => tokens.into(),
        Err(err) => err.to_compile_error().into(),
    }
}

fn expand_instrument(func: ItemFn) -> syn::Result<TokenStream2> {
    let ItemFn {
        attrs,
        vis,
        sig,
        block,
    } = func;

    let fn_name = sig.ident.to_string();

    let mut arg_idents = Vec::with_capacity(sig.inputs.len());
    for input in &sig.inputs {
        match input {
            syn::FnArg::Typed(pat_type) => match &*pat_type.pat {
                syn::Pat::Ident(pat_ident) => arg_idents.push(pat_ident.ident.clone()),
                other => {
                    return Err(syn::Error::new_spanned(
                        other,
                        "#[instrument] requires simple identifier parameters",
                    ))
                }
            },
            syn::FnArg::Receiver(receiver) => {
                return Err(syn::Error::new_spanned(
                    receiver,
                    "#[instrument] does not support methods with `self`",
                ))
            }
        }
    }

    let entry_fmt = {
        let args_fmt = arg_idents
            .iter()
            .map(|id| format!("{id}={{}}"))
            .collect::<Vec<_>>()
            .join(", ");
        format!("-> {fn_name}({args_fmt})")
    };
    let exit_fmt = format!("<- {fn_name} = {{}} ({{}}us)");

    Ok(quote! {
        #(#attrs)*
        #vis #sig {
            println!(#entry_fmt, #(#arg_idents),*);
            let __instrument_start = ::std::time::Instant::now();
            let __instrument_result = (move || #block)();
            let __instrument_elapsed = __instrument_start.elapsed();
            println!(#exit_fmt, __instrument_result, __instrument_elapsed.as_micros());
            __instrument_result
        }
    })
}
