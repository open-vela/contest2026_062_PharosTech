// SPDX-License-Identifier: Apache-2.0

#![allow(dead_code)]

//! Raw Nyabula Core WebAssembly ABI and allocation-free convenience wrappers.

use core::convert::TryFrom;

pub const ABI_VERSION: u32 = 1;
pub type Token = i64;

#[link(wasm_import_module = "nyabula")]
unsafe extern "C" {
    #[link_name = "core_log"]
    fn core_log(message: *const u8, length: u32) -> i32;
    #[link_name = "storage_get"]
    fn storage_get_raw(key: *const u8, key_length: u32, output: *mut u8, capacity: u32) -> i32;
    #[link_name = "storage_put"]
    fn storage_put_raw(key: *const u8, key_length: u32, value: *const u8, length: u32) -> i32;
    #[link_name = "network_request"]
    fn network_request(input: *const u8, input_length: u32, output: *mut u8, capacity: u32) -> i32;
    #[link_name = "ui_notify"]
    fn ui_notify(message: *const u8, length: u32) -> i32;
    #[link_name = "ui_eye"]
    fn ui_eye(command: *const u8, length: u32) -> i32;
    #[link_name = "ai_invoke"]
    fn ai_invoke(prompt: *const u8, prompt_length: u32, output: *mut u8, capacity: u32) -> i32;
    #[link_name = "lifecycle_defer"]
    fn lifecycle_defer() -> Token;
    #[link_name = "lifecycle_complete"]
    fn lifecycle_complete(token: Token, result: i32) -> i32;
    #[link_name = "http_request"]
    fn http_request(url: *const u8, length: u32) -> Token;
    #[link_name = "http_cancel"]
    fn http_cancel(token: Token) -> i32;
}

fn length(value: &[u8]) -> Result<u32, i32> {
    u32::try_from(value.len()).map_err(|_| -7)
}

pub fn log(message: &str) -> i32 {
    length(message.as_bytes()).map_or_else(
        |error| error,
        |size| unsafe { core_log(message.as_ptr(), size) },
    )
}

pub fn storage_get(key: &str, output: &mut [u8]) -> i32 {
    match (length(key.as_bytes()), length(output)) {
        (Ok(key_size), Ok(capacity)) => unsafe {
            storage_get_raw(key.as_ptr(), key_size, output.as_mut_ptr(), capacity)
        },
        _ => -7,
    }
}

pub fn storage_put(key: &str, value: &[u8]) -> i32 {
    match (length(key.as_bytes()), length(value)) {
        (Ok(key_size), Ok(value_size)) => unsafe {
            storage_put_raw(key.as_ptr(), key_size, value.as_ptr(), value_size)
        },
        _ => -7,
    }
}

pub fn notify(message: &str) -> i32 {
    length(message.as_bytes()).map_or_else(
        |error| error,
        |size| unsafe { ui_notify(message.as_ptr(), size) },
    )
}

pub fn eye(command_json: &str) -> i32 {
    length(command_json.as_bytes()).map_or_else(
        |error| error,
        |size| unsafe { ui_eye(command_json.as_ptr(), size) },
    )
}

pub fn invoke(prompt: &str, output: &mut [u8]) -> i32 {
    match (length(prompt.as_bytes()), length(output)) {
        (Ok(prompt_size), Ok(capacity)) => unsafe {
            ai_invoke(prompt.as_ptr(), prompt_size, output.as_mut_ptr(), capacity)
        },
        _ => -7,
    }
}

pub fn defer() -> Token {
    unsafe { lifecycle_defer() }
}

pub fn complete(token: Token, result: i32) -> i32 {
    unsafe { lifecycle_complete(token, result) }
}

pub fn request(url: &str) -> Token {
    length(url.as_bytes()).map_or_else(i64::from, |size| unsafe {
        http_request(url.as_ptr(), size)
    })
}

pub fn cancel(token: Token) -> i32 {
    unsafe { http_cancel(token) }
}

#[allow(dead_code)]
pub fn legacy_network(input: &[u8], output: &mut [u8]) -> i32 {
    match (length(input), length(output)) {
        (Ok(input_size), Ok(capacity)) => unsafe {
            network_request(input.as_ptr(), input_size, output.as_mut_ptr(), capacity)
        },
        _ => -7,
    }
}
