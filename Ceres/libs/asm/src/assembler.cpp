#include <ceres/asm/assembler.h>
#include <format>
#include <fstream>

namespace ceres::casm
{
	std::optional<Program> Assembler::assemble(std::span<const std::filesystem::path> sourceFiles)
	{
		_state.reset();
		_debugInfo = {};
		_state = std::make_unique<AssemblyState>(
			[&](const std::string& filePath) -> OptionalRef<TranslationUnit>
			{
				return loadTranslationUnit(filePath);
			}
		);

		if (sourceFiles.empty())
		{
			reportError("No source files provided for assembly");
			return std::nullopt;
		}

		for (const auto& filePath : sourceFiles)
			loadTranslationUnit(filePath.string());

		if (hasErrors())
			return std::nullopt;

		if (!linkTranslationUnits())
		{
			reportError("Linking failed due to unresolved symbols or other errors");
			return std::nullopt;
		}

		warnAboutUnusedPrivateDeclarations();

		auto binaryOpt = emitBinary();
		if (!binaryOpt.has_value() || hasErrors())
			return std::nullopt;

		return binaryOpt;
	}

	// `global` created a category the language did not have before: a declaration that provably
	// cannot be reached from anywhere else. A private one nobody names in its own file is dead with
	// certainty, which is not something that could be said of anything until now.
	void Assembler::warnAboutUnusedPrivateDeclarations()
	{
		if (!_state)
			return;

		auto& errorHandler = _state->errorHandler();

		for (const auto& unit : _state->translationUnits())
		{
			for (const auto& [name, symbol] : unit.symbolTable().getAllSymbols())
			{
				if (symbol.isGlobal() || symbol.uses() > 0)
					continue;
				if (!symbol.isConstant() && !symbol.isVariable())
					continue; // A label that is never jumped to is often an entry point or a marker.
				// A dotted name is generated, not written: a struct's field offsets, or a local
				// label. Warning about each unused field of a record would drown the useful ones.
				if (name.find('.') != std::string::npos)
					continue;

				errorHandler.reportWarning(unit.file(), symbol.line(), 1,
					std::format("'{}' is declared but never used, and is not global, so nothing outside this file can use it either", name));
			}

			for (const auto& [signature, macro] : unit.macroTable().getAllMacros())
			{
				if (macro.isGlobal() || macro.uses() > 0)
					continue;

				errorHandler.reportWarning(unit.file(), 0, 1,
					std::format("macro '{}' is declared but never used, and is not global", signature.name));
			}
		}
	}

	std::optional<std::string> Assembler::readSourceFile(const std::filesystem::path& filePath)
	{
		try
		{
			std::ifstream file(filePath);
			if (!file.is_open())
			{
				reportError("Could not open source file '{}'", filePath.string());
				return std::nullopt;
			}

			std::string source{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
			return source;
		}
		catch (const std::exception& e)
		{
			reportError("Error reading source file '{}': {}", filePath.string(), e.what());
			return std::nullopt;
		}
	}

	namespace
	{
		// `import "path"` resolves against the importing file, exactly as the translation-unit
		// builder resolves it - the two have to agree or a file would inherit aliases from a
		// module it does not actually import.
		std::string resolveModulePath(const std::filesystem::path& fromFile, std::string_view moduleName)
		{
			std::filesystem::path modulePath{ moduleName };
			if (modulePath.is_relative() && !fromFile.empty())
				modulePath = fromFile.parent_path() / modulePath;
			return modulePath.lexically_normal().string();
		}
	}

	const Assembler::AliasMap& Assembler::collectGlobalAliases(const std::string& resolvedPath)
	{
		if (const auto cached = _globalAliasCache.find(resolvedPath); cached != _globalAliasCache.end())
			return cached->second;

		// A cycle answers with nothing rather than recursing. The import cycle itself is reported
		// where cycles are reported, which is the builder.
		if (!_aliasScanInProgress.insert(resolvedPath).second)
		{
			static const AliasMap empty;
			return empty;
		}

		AliasMap aliases;

		std::optional<std::string> sourceOpt;
		if (auto cachedSource = _state->getSourceFile(resolvedPath); cachedSource.has_value())
		{
			sourceOpt = cachedSource->get();
		}
		else
		{
			// Read straight through rather than cached: a file only scanned for aliases may turn
			// out not to be imported at all, and caching it would put it in the source table under
			// a path nothing else ever asks for.
			std::ifstream file(resolvedPath);
			if (file)
				sourceOpt = std::string{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
		}

		if (sourceOpt.has_value())
		{
			const std::filesystem::path fromFile{ resolvedPath };
			Lexer lexer{ *sourceOpt, _state->stringPool() };

			// Four token shapes are interesting, and everything else is skipped: an import, so
			// its own aliases come along, and `global alias name = register`.
			std::vector<Token> tokens;
			for (Token token = lexer.nextToken(); token.isValid() && !token.isEndOfFile(); token = lexer.nextToken())
				tokens.push_back(token);

			for (usize i = 0; i < tokens.size(); ++i)
			{
				const Token& token = tokens[i];
				if (!token.isKeyword())
					continue;

				if (token.keywordTypeValue() == KeywordType::Import && i + 1 < tokens.size() && tokens[i + 1].isLiteralString())
				{
					const std::string importedPath = resolveModulePath(fromFile, tokens[i + 1].literalStringValue().view());
					for (const auto& [name, operand] : collectGlobalAliases(importedPath))
						aliases.insert_or_assign(name, operand);
					continue;
				}

				if (token.keywordTypeValue() != KeywordType::Global)
					continue;
				if (i + 4 >= tokens.size())
					continue;
				if (!tokens[i + 1].isKeyword() || tokens[i + 1].keywordTypeValue() != KeywordType::Alias)
					continue;
				if (!tokens[i + 2].isIdentifier() || !tokens[i + 3].isEquals() || !tokens[i + 4].isIdentifier())
					continue;

				const auto registerInfo = RegisterInfo::get(tokens[i + 4].identifierValue());
				if (!registerInfo.has_value())
					continue; // Not a register: the parser reports it when it reaches that line

				aliases.insert_or_assign(std::string(tokens[i + 2].lexeme()), registerInfo->isFloatingPoint
					? Operand::makeFloatingPointRegister(registerInfo->index)
					: Operand::makeRegister(registerInfo->index));
			}
		}

		_aliasScanInProgress.erase(resolvedPath);
		return _globalAliasCache.insert_or_assign(resolvedPath, std::move(aliases)).first->second;
	}

	// What a file being parsed inherits: the union of what each of its imports publishes. Its own
	// `global alias` declarations are not in here - the parser reaches those on its own way down.
	Assembler::AliasMap Assembler::collectImportedAliases(const std::string& resolvedPath)
	{
		AliasMap aliases;

		auto source = _state->getSourceFile(resolvedPath);
		if (!source.has_value())
			return aliases;

		const std::filesystem::path fromFile{ resolvedPath };
		Lexer lexer{ source->get(), _state->stringPool() };

		Token previous;
		for (Token token = lexer.nextToken(); token.isValid() && !token.isEndOfFile(); token = lexer.nextToken())
		{
			if (previous.isKeyword() && previous.keywordTypeValue() == KeywordType::Import && token.isLiteralString())
			{
				for (const auto& [name, operand] : collectGlobalAliases(resolveModulePath(fromFile, token.literalStringValue().view())))
					aliases.insert_or_assign(name, operand);
			}
			previous = token;
		}

		return aliases;
	}

	std::vector<Statement> Assembler::parseSource(const std::string& source, const std::filesystem::path& filePath)
	{
		try
		{
			// A view into the source-file cache's stable key, not filePath.string() directly - that
			// would be a temporary, and every Statement the parser builds holds this view onward
			// without copying it.
			// Whatever this file's imports publish, in hand before the first line is parsed.
			const AliasMap imported = collectImportedAliases(filePath.string());

			Parser parser{ source, _state->stringPool(), _state->errorHandler(), _state->internedPath(filePath.string()), &imported };
			return parser.parse();
		}
		catch (const std::exception& e)
		{
			reportError("Error parsing source file '{}': {}", filePath.string(), e.what());
			return {};
		}
	}

	std::optional<TranslationUnit> Assembler::translateStatementsToUnit(const std::string& source, std::vector<Statement>&& statements, const std::filesystem::path& filePath)
	{
		try
		{
			TranslationUnitBuilder builder{ *_state, filePath, _state->internedPath(filePath.string()) };
			builder.build(std::move(statements));
			return builder.release();
		}
		catch (const std::exception& e)
		{
			reportError("Error translating statements to translation unit for file '{}': {}", filePath.string(), e.what());
			return std::nullopt;
		}
	}

	std::optional<ObjectFile> Assembler::assembleObject(const std::filesystem::path& sourceFile)
	{
		_state.reset();
		_debugInfo = {};
		_state = std::make_unique<AssemblyState>(
			[&](const std::string& filePath) -> OptionalRef<TranslationUnit>
			{
				return loadTranslationUnit(filePath);
			}
		);

		loadTranslationUnit(sourceFile.string());
		if (hasErrors())
			return std::nullopt;

		// Only valid once the file has been read: the view points into the source cache own key.
		const std::string_view rootFile = _state->internedPath(sourceFile.string());

		if (!linkTranslationUnitsForObject(rootFile))
		{
			reportError("Assembling '{}' failed due to unresolved symbols or other errors", sourceFile.string());
			return std::nullopt;
		}

		warnAboutUnusedPrivateDeclarations();

		const TranslationUnit* root = nullptr;
		for (const auto& unit : _state->translationUnits())
		{
			if (unit.file() == rootFile)
				root = &unit;
		}

		if (root == nullptr)
		{
			reportError("Could not assemble '{}'", sourceFile.string());
			return std::nullopt;
		}

		try
		{
			// No entry point is required: an object is a piece of a program, and which piece holds
			// `main` is the link's business.
			BinaryEmitter emitter{ *_state, _options.emitDebugInfo, false, rootFile };
			const auto emitted = emitter.emit();
			if (!emitted.has_value() || hasErrors())
				return std::nullopt;

			ObjectFile object;
			object.sourceFile = sourceFile.string();
			object.text.assign(emitter.textBuffer().begin(), emitter.textBuffer().end());
			object.rodata.assign(emitter.rodataBuffer().begin(), emitter.rodataBuffer().end());
			object.data.assign(emitter.dataBuffer().begin(), emitter.dataBuffer().end());
			object.bssSize = _state->memoryMap().bssSize;
			object.relocations = emitter.takeRelocations();

			// Only what another object could name. A private label is at a known place inside this
			// object and the relocations that reach it already say so; publishing it would only
			// give two files a chance to collide over a name neither meant to share.
			for (const auto& [name, symbol] : root->symbolTable().getAllSymbols())
			{
				if (!symbol.isGlobal() || symbol.isConstant() || !symbol.hasAddress())
					continue;

				object.symbols.push_back(ObjectSymbol{
					std::string(name), symbol.section(), symbol.address().value() });
			}

			if (_options.emitDebugInfo)
			{
				_debugInfo = emitter.takeDebugInfo();
				object.debugSection = _debugInfo.serialize();
			}

			return object;
		}
		catch (const std::exception& e)
		{
			reportError("Error emitting object file: {}", e.what());
			return std::nullopt;
		}
	}

	bool Assembler::linkTranslationUnits()
	{
		try
		{
			Linker linker{ *_state };
			return linker.link();
		}
		catch (const std::exception& e)
		{
			reportError("Error linking translation units: {}", e.what());
			return false;
		}
	}

	bool Assembler::linkTranslationUnitsForObject(std::string_view rootFile)
	{
		try
		{
			Linker linker{ *_state };
			return linker.linkObject(rootFile);
		}
		catch (const std::exception& e)
		{
			reportError("Error laying out translation units: {}", e.what());
			return false;
		}
	}

	std::optional<Program> Assembler::emitBinary()
	{
		try
		{
			BinaryEmitter emitter{ *_state, _options.emitDebugInfo, _options.requireEntryPoint };
			auto program = emitter.emit();
			if (program.has_value())
				_debugInfo = emitter.takeDebugInfo();
			return program;
		}
		catch (const std::exception& e)
		{
			reportError("Error emitting binary: {}", e.what());
			return std::nullopt;
		}
	}

	OptionalRef<TranslationUnit> Assembler::loadTranslationUnit(const std::string& filePath) noexcept
	{
		if (!_state)
			return std::nullopt;

		auto result = _state->getTranslationUnit(filePath);
		if (result.has_value())
			return result;

		auto sourceOpt = readSourceFile(filePath);
		if (!sourceOpt.has_value())
			return std::nullopt;

		auto& source = _state->cacheSourceFile(filePath, std::move(sourceOpt.value()));

		auto statements = parseSource(source, filePath);
		if (hasErrors())
			return std::nullopt;

		// Marked in progress across the build so a circular import is reported rather than
		// recursing until the stack runs out.
		_state->beginLoading(filePath);
		auto translationUnit = translateStatementsToUnit(source, std::move(statements), filePath);
		_state->endLoading(filePath);

		if (!translationUnit.has_value())
			return std::nullopt;

		return _state->cacheTranslationUnit(filePath, std::move(translationUnit.value()));
	}
}
