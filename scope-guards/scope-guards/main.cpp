#include "scope_guards.h"
#include <atomic>
#include <cassert>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

using Handle = uint64_t;

extern "C" {
// Получаем ресурс
Handle AcquireResource()
{
	static std::atomic<Handle> nextHandle{ 0 };
	auto handle = ++nextHandle;
	std::cout << "Acquired resource " << handle << "\n";
	return handle;
}

// Использовать ресурс
void UseResource(Handle h)
{
	std::cout << "Using resource " << h << "\n";
}

// Освобождаем ресурс
void ReleaseResource(Handle h)
{
	std::cout << "Released resource " << h << "\n";
}
}

void FailingFunction()
{
	if (true)
		throw std::runtime_error("Boom!");
}
namespace automatic
{

class ResourceGuard
{ // RAII-обертка для ресурса
public:
	ResourceGuard()
		: m_handle(AcquireResource())
	{ // Конструктор принимает владение ресурсом
		if (!m_handle)
			throw std::runtime_error("Failed to acquire resource");
	}

	~ResourceGuard()
	{ // Деструктор освобождает ресурс
		ReleaseResource(m_handle);
	}

	// Запрещаем копирование и присваивание
	ResourceGuard(const ResourceGuard&) = delete;
	ResourceGuard& operator=(const ResourceGuard&) = delete;

	// В перемещающем конструкторе ресурс передается от другого объекта
	ResourceGuard(ResourceGuard&& other) noexcept
		: m_handle(std::exchange(other.m_handle, 0))
	{
	}

	// Перемещающее присваивание
	ResourceGuard& operator=(ResourceGuard&& other) noexcept
	{
		if (this != &other) // Защита от самоприсваивания
		{
			if (m_handle != 0) // Освобождаем текущий ресурс
				ReleaseResource(m_handle);
			m_handle = std::exchange(other.m_handle, 0); // Перемещаем ресурс из other
		}
		return *this;
	}

	void Use()
	{
		assert(m_handle != 0 && "Resource already released");
		UseResource(m_handle);
	}

	Handle Get() const noexcept // Получить "сырой" дескриптор ресурса
	{
		return m_handle;
	}

	[[nodiscard]] Handle Release() noexcept
	{ // Отказаться от владения ресурсом
		return std::exchange(m_handle, 0);
	}

private:
	Handle m_handle;
};

void DoWork()
{
	ResourceGuard res;
	if (true)
		return;
	res.Use();
	FailingFunction();
}

} // namespace automatic

namespace manual
{
void DoWork()
{
	const Handle h = AcquireResource();
	try
	{
		if (true)
		{
			ReleaseResource(h);
			return;
		}

		UseResource(h);

		FailingFunction();

		ReleaseResource(h);
	}
	catch (...)
	{
		ReleaseResource(h);
		throw;
	}
}
} // namespace manual

namespace sg1
{

template <class Fn> // Fn — тип функции, которая освобождает ресурс
class ScopeExit
{
public:
	explicit ScopeExit(Fn fn) noexcept
		: m_fn(std::move(fn))
	{
	}

	// Запрещаем копирование и присваивание
	ScopeExit(const ScopeExit&) = delete;
	ScopeExit& operator=(const ScopeExit&) = delete;

	ScopeExit(ScopeExit&& other) noexcept // Разрешен только перемещающий конструктор
		: m_fn(std::move(other.m_fn))
		, m_active(std::exchange(other.m_active, false))
	{
	}

	~ScopeExit() noexcept
	{
		// В деструкторе вызываем функцию, если охранник активен
		if (m_active)
			m_fn();
	}

	void Release() noexcept { m_active = false; }

private:
	Fn m_fn;
	bool m_active{ true };
};

void DoWork()
{
	const Handle h = AcquireResource();

	// Создаем объект-охранник и передаем лямбду, которая освободит ресурс
	ScopeExit cleanup([h]() noexcept {
		ReleaseResource(h);
	});

	if (true)
		return; // Ресурс будет освобожден охранником при досрочном выходе

	UseResource(h);

	FailingFunction(); // Если здесь будет исключение, деструктор охранника освободит ресурс

	if (true)
	{
		ReleaseResource(h); // Освобождаем ресурс вручную
		cleanup.Release(); // Отказываемся от автоматического освобождения ресурса
	}

	// Ресурс будет освобожден при выходе из функции, если не был освобожден вручную
}

} // namespace sg1

namespace sg2
{

template <class Fn>
concept GuardFn = std::is_nothrow_invocable_v<Fn>
	&& std::is_nothrow_move_constructible_v<Fn>;

template <GuardFn Fn>
class ScopeExit
{
public:
	explicit ScopeExit(Fn fn) noexcept
		: m_fn(std::move(fn))
	{
	}

	// Запрещаем копирование и присваивание
	ScopeExit(const ScopeExit&) = delete;
	ScopeExit& operator=(const ScopeExit&) = delete;

	ScopeExit(ScopeExit&& other) noexcept // Разрешен только перемещающий конструктор
		: m_fn(std::move(other.m_fn))
		, m_active(std::exchange(other.m_active, false))
	{
	}

	~ScopeExit() noexcept
	{
		// В деструкторе вызываем функцию, если охранник активен
		if (m_active)
			m_fn();
	}

	void Release() noexcept { m_active = false; }

private:
	[[no_unique_address]] Fn m_fn;
	bool m_active{ true };
};

void DoWork()
{
	const Handle h = AcquireResource();

	ScopeExit cleanup{ [h]() noexcept {
		ReleaseResource(h);
	} };

	ScopeExit note{ []() noexcept {
		std::cout << "Exiting DoWork\n";
	} };
}

class OrderedDictionary
{
public:
	const std::string& Get(std::string_view key) const
	{
		auto it = m_map.find(key);
		if (it == m_map.end())
			throw std::out_of_range("Key not found");
		return it->second;
	}

	bool TryAdd(std::string key, std::string value)
	{
#if 1
		auto [it, inserted] = m_map.emplace(std::move(key), std::move(value));
		if (!inserted)
			return false; // Ключ уже существует

		ScopeExit rollback{ [&]() noexcept {
			m_map.erase(it);
		} };

		m_keys.emplace_back(it->first);

		rollback.Release(); // Добавление успешно, отменяем откат
#else
		auto [it, inserted] = m_map.emplace(std::move(key), std::move(value));
		if (!inserted)
			return false; // Ключ уже существует

		try
		{
			m_keys.emplace_back(it->first);
		}
		catch (...)
		{
			m_map.erase(it); // Откат при ошибке
			throw;
		}

#endif
		return true;
	}

	const std::string& operator[](size_t index) const
	{
		if (index >= m_keys.size())
			throw std::out_of_range("Index out of range");
		return Get(m_keys[index]);
	}

private:
	struct Hasher
	{
		using is_transparent = void;

		size_t operator()(std::string_view key) const noexcept
		{
			return std::hash<std::string_view>{}(key);
		}
	};
	struct Comparator
	{
		using is_transparent = void;

		bool operator()(std::string_view a, std::string_view b) const noexcept
		{
			return a == b;
		}
	};
	std::unordered_map<std::string, std::string, Hasher, Comparator> m_map;
	std::vector<std::string_view> m_keys;
};

void TestDictionary()
{
	OrderedDictionary dict;
	dict.TryAdd("one", "1"); // 0
	dict.TryAdd("two", "2"); // 1
	assert(dict[1] == "2");
}

void UncaughtExceptionsTests()
{
	struct ExceptionCounter
	{
		ExceptionCounter()
		{
			std::cout << "Uncaught exceptions at start: " << std::uncaught_exceptions() << "\n";
		}
		~ExceptionCounter()
		{
			std::cout << "Uncaught exceptions at end: " << std::uncaught_exceptions() << "\n";
		}
	};

	try
	{
		{
			ExceptionCounter counter{};
		}

		ExceptionCounter counter;
		throw std::runtime_error("Test");
	}
	catch (...)
	{
		std::cout << "Caught exception\n";
	}
}

} // namespace sg2

namespace sg3
{
namespace detail
{

template <class Fn>
concept GuardFn = std::is_nothrow_invocable_v<Fn> && std::is_nothrow_move_constructible_v<Fn>;

template <GuardFn Fn>
class ScopeGuardBase
{
public:
	explicit ScopeGuardBase(Fn&& fn) noexcept
		: m_fn(std::move(fn))
	{
	}

	ScopeGuardBase(const ScopeGuardBase&) = delete;
	ScopeGuardBase& operator=(const ScopeGuardBase&) = delete;

	ScopeGuardBase(ScopeGuardBase&& other) noexcept
		: m_fn(std::move(other.m_fn))
		, m_active(std::exchange(other.m_active, false))
	{
	}

	ScopeGuardBase& operator=(ScopeGuardBase&&) = delete;

	void Release() noexcept { m_active = false; }
	[[nodiscard]] bool Active() const noexcept { return m_active; }

protected:
	void ExecFn() noexcept
	{
		m_fn();
	}

private:
	[[no_unique_address]] Fn m_fn;
	bool m_active{ true };
};

template <GuardFn Fn>
class ScopeTryBase : public ScopeGuardBase<Fn>
{
	using Base = ScopeGuardBase<Fn>;

public:
	using Base::Base;

protected:
	int m_enter = std::uncaught_exceptions();
};

} // namespace detail

template <detail::GuardFn Fn>
class ScopeExit final : public detail::ScopeGuardBase<Fn>
{
	using Base = detail::ScopeGuardBase<Fn>;

public:
	using Base::Base;

	// Явно просим компилятор сгенерировать перемещающий конструктор,
	// так как при наличии деструктора он не будет этого делать.
	ScopeExit(ScopeExit&& other) noexcept = default;

	~ScopeExit()
	{
		if (this->Active())
		{
			this->ExecFn();
		}
	}
};

template <detail::GuardFn Fn>
class ScopeSuccess final : public detail::ScopeTryBase<Fn>
{
	using Base = detail::ScopeTryBase<Fn>;

public:
	using Base::Base;

	ScopeSuccess(ScopeSuccess&& other) noexcept = default;

	~ScopeSuccess()
	{
		if (this->Active() && std::uncaught_exceptions() == this->m_enter)
		{
			this->ExecFn();
		}
	}
};

template <detail::GuardFn Fn>
class ScopeFail final : public detail::ScopeTryBase<Fn>
{
	using Base = detail::ScopeTryBase<Fn>;

public:
	using Base::Base;

	ScopeFail(ScopeFail&& other) noexcept = default;

	~ScopeFail()
	{
		if (this->Active() && std::uncaught_exceptions() > this->m_enter)
		{
			this->ExecFn();
		}
	}
};

#if 0
void WillNotCompile()
{
	ScopeExit guard{ []() noexcept {
		std::cout << "Exiting scope\n";
	} };
}
#endif

template <class F>
ScopeExit(F) -> ScopeExit<F>;

template <class F>
ScopeSuccess(F) -> ScopeSuccess<F>;

template <class F>
ScopeFail(F) -> ScopeFail<F>;

void Ok()
{
	ScopeExit guard{ []() noexcept {
		std::cout << "Exiting scope\n";
	} };
}

// Фабрики упрощающие создание охранников

template <class F>
// decay_t - убирает ссылки и const/volatile-квалификаторы с типа F
	requires detail::GuardFn<std::decay_t<F>>
[[nodiscard]] constexpr auto MakeScopeExit(F&& f) noexcept -> ScopeExit<std::decay_t<F>>
{
	return ScopeExit<std::decay_t<F>>{ std::forward<F>(f) };
}

template <class F>
	requires detail::GuardFn<std::decay_t<F>>
[[nodiscard]] constexpr auto MakeScopeSuccess(F&& f) noexcept -> ScopeSuccess<std::decay_t<F>>
{
	return ScopeSuccess<std::decay_t<F>>{ std::forward<F>(f) };
}

template <class F>
	requires detail::GuardFn<std::decay_t<F>>
[[nodiscard]] constexpr auto MakeScopeFail(F&& f) noexcept -> ScopeFail<std::decay_t<F>>
{
	return ScopeFail<std::decay_t<F>>{ std::forward<F>(f) };
}

void FactoryVsDirect()
{
#if 0
	auto guard1 = MakeScopeExit([] {
		std::cout << "Exiting scope (factory)\n";
	});
#endif
#if 0
	ScopeExit guard2{ [] {
		std::cout << "Exiting scope (direct)\n";
	} };
#endif
}

class OrderedDictionary
{
public:
	const std::string& Get(std::string_view key) const
	{
		auto it = m_map.find(key);
		if (it == m_map.end())
			throw std::out_of_range("Key not found");
		return it->second;
	}

	bool TryAdd(std::string key, std::string value)
	{
		auto [it, inserted] = m_map.emplace(std::move(key), std::move(value));
		if (!inserted)
			return false; // Ключ уже существует

		ScopeFail rollbackOnFail{ [&]() noexcept {
			m_map.erase(it);
		} };

		m_keys.emplace_back(it->first);

		return true;
	}

	const std::string& operator[](size_t index) const
	{
		if (index >= m_keys.size())
			throw std::out_of_range("Index out of range");
		return Get(m_keys[index]);
	}

private:
	struct Hasher
	{
		using is_transparent = void;

		size_t operator()(std::string_view key) const noexcept
		{
			return std::hash<std::string_view>{}(key);
		}
	};
	struct Comparator
	{
		using is_transparent = void;

		bool operator()(std::string_view a, std::string_view b) const noexcept
		{
			return a == b;
		}
	};
	std::unordered_map<std::string, std::string, Hasher, Comparator> m_map;
	std::vector<std::string_view> m_keys;
};

} // namespace sg3

void PrintInHex()
{
	{
		const auto oldFlags = std::cout.flags();
		auto restoreFlags = sg::MakeScopeExit([&]() noexcept { std::cout.flags(oldFlags); });

		// Выведем числа в шестнадцатеричном формате с префиксом 0x
		std::cout << std::hex << std::showbase << 10 << ", " << 20 << ", " << 1000 << "\n";

		// При выходе из блока restoreFlags восстановит старые флаги
	}
	// Выведем числа в обычном десятичном формате
	std::cout << 10 << ", " << 20 << ", " << 1000 << "\n";
}

namespace fs = std::filesystem;

void WriteFileAtomically(const fs::path& path, std::string_view data)
{
	auto tmpName = path.string() + ".tmp";
	bool ready = false; // Флаг, сигнализирующий guard-ам о начале работы

	auto removeOnFail = sg::MakeScopeFail([&]() noexcept {
		if (!ready)
			return;
		std::error_code ec;
		fs::remove(tmpName, ec);
	});

	auto renameOnSuccess = sg::MakeScopeSuccess([&]() noexcept {
		if (!ready)
			return;
		std::error_code ec;
		fs::rename(tmpName, path, ec); // Игнорируем ошибки переименования
	});

	// Конструируем файл после guard-ов, чтобы он закрылся ДО переименования/удаления
	std::ofstream ofs(tmpName, std::ios::binary);
	if (!ofs)
		throw std::runtime_error("Failed to open temporary file for writing");

	ready = true; // С этого момента guard-ы активны
	ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
	ofs.flush(); // Форсируем запись данных

	if (!ofs) // Проверяем успех записи
		throw std::runtime_error("Failed to write data to temporary file");

	// Здесь может быть ещё код, который может бросить исключение или выйти досрочно

	// Файл закрывается и сработают guard-ы
}

int main()
{
	try
	{
		PrintInHex();
		// sg2::UncaughtExceptionsTests();
	}
	catch (const std::exception& e)
	{
		std::cerr << "Error: " << e.what() << "\n";
		return 1;
	}
	catch (...)
	{
		std::cerr << "Unknown error\n";
	}
	return 0;
}
