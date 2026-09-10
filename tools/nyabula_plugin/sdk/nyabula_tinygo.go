// SPDX-License-Identifier: Apache-2.0

package nyabula

import "unsafe"

const ABIVersion = 1

type Token = int64

//go:wasmimport nyabula core_log
func coreLog(message unsafe.Pointer, length uint32) int32

//go:wasmimport nyabula storage_get
func storageGet(key unsafe.Pointer, keyLength uint32,
	output unsafe.Pointer, capacity uint32) int32

//go:wasmimport nyabula storage_put
func storagePut(key unsafe.Pointer, keyLength uint32,
	value unsafe.Pointer, valueLength uint32) int32

//go:wasmimport nyabula ui_notify
func uiNotify(message unsafe.Pointer, length uint32) int32

//go:wasmimport nyabula ui_eye
func uiEye(command unsafe.Pointer, length uint32) int32

//go:wasmimport nyabula ai_invoke
func aiInvoke(prompt unsafe.Pointer, promptLength uint32,
	output unsafe.Pointer, capacity uint32) int32

//go:wasmimport nyabula lifecycle_defer
func lifecycleDefer() Token

//go:wasmimport nyabula lifecycle_complete
func lifecycleComplete(token Token, result int32) int32

//go:wasmimport nyabula http_request
func httpRequest(url unsafe.Pointer, length uint32) Token

//go:wasmimport nyabula http_cancel
func httpCancel(token Token) int32

func bytesPointer(value []byte) unsafe.Pointer {
	if len(value) == 0 {
		return unsafe.Pointer(nil)
	}
	return unsafe.Pointer(&value[0])
}

func Log(message string) int32 {
	data := []byte(message)
	return coreLog(bytesPointer(data), uint32(len(data)))
}

func StorageGet(key string, output []byte) int32 {
	keyData := []byte(key)
	return storageGet(bytesPointer(keyData), uint32(len(keyData)),
		bytesPointer(output), uint32(len(output)))
}

func StoragePut(key string, value []byte) int32 {
	keyData := []byte(key)
	return storagePut(bytesPointer(keyData), uint32(len(keyData)),
		bytesPointer(value), uint32(len(value)))
}

func Notify(message string) int32 {
	data := []byte(message)
	return uiNotify(bytesPointer(data), uint32(len(data)))
}

func Eye(commandJSON string) int32 {
	data := []byte(commandJSON)
	return uiEye(bytesPointer(data), uint32(len(data)))
}

func Invoke(prompt string, output []byte) int32 {
	promptData := []byte(prompt)
	return aiInvoke(bytesPointer(promptData), uint32(len(promptData)),
		bytesPointer(output), uint32(len(output)))
}

func Defer() Token {
	return lifecycleDefer()
}

func Complete(token Token, result int32) int32 {
	return lifecycleComplete(token, result)
}

func Request(url string) Token {
	data := []byte(url)
	return httpRequest(bytesPointer(data), uint32(len(data)))
}

func Cancel(token Token) int32 {
	return httpCancel(token)
}
