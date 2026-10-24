#include "mysql.hpp"

#include <argon2.h>

extern "C" {
#include <crypt_blowfish.h>
}

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <fstream>
#endif

namespace mysqlcomponent {

namespace {

constexpr size_t PasswordThreads = 2;
constexpr size_t SaltBytes = 16;
constexpr size_t Argon2Bytes = 32;

bool randomBytes(unsigned char* output, size_t size) {
#ifdef _WIN32
	return BCryptGenRandom(nullptr, output, ULONG(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
	std::ifstream random("/dev/urandom", std::ios::binary);
	return bool(random.read(reinterpret_cast<char*>(output), std::streamsize(size)));
#endif
}

void wipe(std::string& text) {
	volatile char* bytes = text.data();
	for (size_t index = 0; index != text.size(); ++index) {
		bytes[index] = 0;
	}
	text.clear();
}

bool startsWith(const std::string& text, const char* prefix) {
	return text.compare(0, std::strlen(prefix), prefix) == 0;
}

bool sameText(const char* computed, const std::string& stored) {
	size_t length = std::strlen(computed);
	unsigned char difference = length == stored.size() ? 0 : 1;
	for (size_t index = 0; index != stored.size(); ++index) {
		difference |= (unsigned char)(index < length ? computed[index] : 0) ^ (unsigned char)stored[index];
	}
	return difference == 0;
}

void hashArgon2(PasswordJob& job) {
	unsigned char salt[SaltBytes];
	if (!randomBytes(salt, sizeof salt)) {
		job.error = "the system gave no random bytes";
		return;
	}
	std::string encoded(argon2_encodedlen(job.iterations, job.memory, job.parallelism, SaltBytes, Argon2Bytes, Argon2_id), '\0');
	int status = argon2id_hash_encoded(job.iterations, job.memory, job.parallelism, job.password.data(), job.password.size(), salt, SaltBytes, Argon2Bytes, encoded.data(), encoded.size());
	if (status != ARGON2_OK) {
		job.error = argon2_error_message(status);
		return;
	}
	encoded.resize(std::strlen(encoded.c_str()));
	job.hash = std::move(encoded);
}

void hashBcrypt(PasswordJob& job) {
	unsigned char random[SaltBytes];
	char setting[32];
	char output[64];
	if (!randomBytes(random, sizeof random)) {
		job.error = "the system gave no random bytes";
		return;
	}
	if (_crypt_gensalt_blowfish_rn("$2b$", job.cost, reinterpret_cast<const char*>(random), int(sizeof random), setting, int(sizeof setting)) == nullptr) {
		job.error = "the bcrypt cost is not valid";
		return;
	}
	if (_crypt_blowfish_rn(job.password.c_str(), setting, output, int(sizeof output)) == nullptr) {
		job.error = "bcrypt failed";
		return;
	}
	job.hash = output;
}

void verify(PasswordJob& job) {
	const std::string& hash = job.hash;
	if (startsWith(hash, "$argon2")) {
		argon2_type type = Argon2_id;
		if (startsWith(hash, "$argon2i$")) {
			type = Argon2_i;
		} else if (startsWith(hash, "$argon2d$")) {
			type = Argon2_d;
		} else if (!startsWith(hash, "$argon2id$")) {
			job.error = "the hash is not one of Argon2";
			return;
		}
		int status = argon2_verify(hash.c_str(), job.password.data(), job.password.size(), type);
		job.match = status == ARGON2_OK;
		if (status != ARGON2_OK && status != ARGON2_VERIFY_MISMATCH) {
			job.error = argon2_error_message(status);
		}
		return;
	}
	if (startsWith(hash, "$2a$") || startsWith(hash, "$2b$") || startsWith(hash, "$2x$") || startsWith(hash, "$2y$")) {
		char output[64];
		if (_crypt_blowfish_rn(job.password.c_str(), hash.c_str(), output, int(sizeof output)) == nullptr) {
			job.error = "the bcrypt hash is not valid";
			return;
		}
		job.match = sameText(output, hash);
		return;
	}
	job.error = "the hash is neither Argon2 nor bcrypt";
}

}

Passwords::~Passwords() {
	stop();
}

void Passwords::submit(std::unique_ptr<PasswordJob> job) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_) {
			return;
		}
		if (threads_.empty()) {
			for (size_t index = 0; index != PasswordThreads; ++index) {
				threads_.emplace_back([this]() {
					work();
				});
			}
		}
		queue_.push_back(std::move(job));
	}
	wake_.notify_one();
}

void Passwords::collect(std::vector<std::unique_ptr<PasswordJob>>& jobs) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (finished_.empty()) {
		return;
	}
	for (std::unique_ptr<PasswordJob>& job : finished_) {
		jobs.push_back(std::move(job));
	}
	finished_.clear();
}

void Passwords::stop() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
	}
	wake_.notify_all();
	for (std::thread& thread : threads_) {
		if (thread.joinable()) {
			thread.join();
		}
	}
	threads_.clear();
}

void Passwords::work() {
	for (;;) {
		std::unique_ptr<PasswordJob> job;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this]() {
				return stopping_ || !queue_.empty();
			});
			if (stopping_) {
				return;
			}
			job = std::move(queue_.front());
			queue_.pop_front();
		}
		switch (job->work) {
			case PasswordWork::Argon2:
				hashArgon2(*job);
				break;
			case PasswordWork::Bcrypt:
				hashBcrypt(*job);
				break;
			case PasswordWork::Verify:
				verify(*job);
				break;
		}
		wipe(job->password);
		std::lock_guard<std::mutex> lock(mutex_);
		finished_.push_back(std::move(job));
	}
}

void dispatchPasswords(std::vector<std::unique_ptr<PasswordJob>>& jobs) {
	for (std::unique_ptr<PasswordJob>& job : jobs) {
		if (!job->error.empty()) {
			logError("[MySQL] A password could not be %s: %s.", job->work == PasswordWork::Verify ? "verified" : "hashed", job->error.c_str());
		}
		Script* script = scriptOf(job->amx);
		if (script == nullptr || script->serial != job->owner) {
			continue;
		}
		int index = -1;
		if (!findPublic(*script, job->callback.c_str(), index)) {
			continue;
		}
		cell text = 0;
		if (job->work == PasswordWork::Verify) {
			script->api->Push(job->match ? 1 : 0);
		} else {
			script->api->PushString(&text, nullptr, StringView(job->hash.data(), job->hash.size()), false, false);
		}
		script->api->Push(job->context);
		cell ignored = 0;
		script->api->Exec(&ignored, index);
		if (text != 0) {
			script->api->Release(text);
		}
	}
}

}
