/*
 * cliEngine.cpp
 *
 *  Created on: 16.03.2009
 *      Author: ast
 */

#include <vector>
#include <string>
#include <iostream>
#include <string.h>

#include <readline/history.h>
#include <readline/readline.h>

#include <boost/algorithm/string/trim.hpp>

#include "cliApi.h"

#include "cliCommand.h"
#include "cliEngine.h"
#include "cliUtils.h"
#include "auxilary.h"
#include "adtauth.h"
#include "adtstring.h"

#include "internalCmd.h"

using namespace std;

namespace CLI {


	Engine::ContextCommandStorageType_t Engine::commands_;
	ContextModuleHolderType_t Engine::holder_;
	Context_t Engine::context_;
	bool stop_to_work = false;

	/* Forward declaration */
namespace {

	std::vector<std::string> tokens;
	typedef std::vector<std::string>::const_iterator TokenConstIt_t;
	ParameterStorageType_t paramStorage;
	std::vector<std::string> contextHelp;

	AdtAuth::AdtGroup currentGroup;
	AdtAuth::AdtUser currentUser;

	std::string cmdText;

	int QuestionMarkKeyMap(int , int);
	char ** UserCompletion(const char* text, int start, int end);
	char * Generator(const char*  text, int  state);

	void split_into_tokens_int (const std::string& stream, BasicStringContainer_t& array);

	class LookupFunctor
	{
		public:
				// Creates a functor and memorises tokens
			LookupFunctor( const vector< string > &  tokens, CommandError_t & cmdError ) :
				tokens_( tokens ), cmdError_(cmdError)
			{}

			bool operator()( const Engine::ElementType_t&  elem ) const
			{
				CommandPtr_t cmd = elem.second;

				return cmd->validate(tokens_, paramStorage, cmdError_);

			}

		private:
			const vector< string > &  tokens_;
			CommandError_t& cmdError_;
	};


	class ContextFunctor
	{
		public:
				// Creates a functor and memorises tokens
			ContextFunctor( const vector< string > &  tokens , CommandError_t & cmdError ) :
				tokens_( tokens ), cmdError_(cmdError)
			{
				contextHelp.clear();
			}

			void operator()( const Engine::ElementType_t&  elem) const
			{

				CommandPtr_t cmd = elem.second;

				cmd->getContextHelp(tokens_, contextHelp);

				bool canBeCompleted = cmd->validate(tokens_, paramStorage, cmdError_);

				if (canBeCompleted == true)
				{
					contextHelp.push_back("<cr>");
				}
			}

		private:
			const vector< string > &  tokens_;
			CommandError_t & cmdError_;
	};

	/*
	 *  hook to printout context help
	 *
	 */
	int preinputhook()
	{
		rl_insert_text(cmdText.c_str());
		rl_redisplay();
		return 0;
	}
} // namespace

/*****************************************************************************/
/*                              CLI::Engine                                  */
/*****************************************************************************/
void registerCommand(const ModulePtr_t& module,
		const CommandPtr_t& command,
		securityHook hook,
		Context_t context)
{
	// TODO: current group should be an array
	// here we should use foreach
	if (hook (currentGroup.name()) == true || currentUser.isRoot() == true)
	{
		Engine::Instance().registerCommand(module, command, context);
	}
#if 0
	if (currentUser.isMemberOfGroup(AdtAuth::ADT_ADMIN) ||
	   currentUser.isMemberOfGroup(AdtAuth::ADT_ROOT) || security == AdtAuth::ADT_ANY
	   )
	{
		Engine::Instance().registerCommand(module, command);
	}
	else
	{

		if (currentUser.isMemberOfGroup(security))
			Engine::Instance().registerCommand(module, command);
	}
#endif
}

ModulePtr_t createModule(const std::string& module, const std::string& help, Context_t context)
{
	return Engine::Instance().registerModule(module, help, context);
}



Engine::Engine()
{
	::using_history();
	::rl_bind_key('?', QuestionMarkKeyMap);
	::rl_variable_bind("print-completions-horizontally", "off");
	rl_attempted_completion_function = UserCompletion;
	context_ = CLI_CTX_NORMAL;
}


void printErrorMsg (std::vector<std::string>& tokens, const std::string& prompt, const CommandError_t&  cmdError)
{
	int count = printf("%s: ", prompt.c_str());

	TokenConstIt_t begin = tokens.begin();
	TokenConstIt_t end = tokens.end();

	string spacer;
	string output;

	for (; begin != end; ++begin)
	{
		count += printf ("%s", spacer.c_str());
		if (begin == cmdError.position)
		{
			output.insert(output.begin(), count, ' ');
		}
		count += printf ("%s", (*begin).c_str());
		spacer=" ";
	}
	printf("\n");
	printf("%s^\n", output.c_str());
}

void processErrorMsg(const CommandError_t&  cmdError)
{
	switch (cmdError.error)
	{
	case CLI_CMD_SHORT:
		std::cout << TR("Incomplete command") << std::endl;
		break;
	case CLI_CMD_TOO_LONG:
	{
		printErrorMsg(tokens,TR("Too many parameters"), cmdError);
		break;
	}
	case CLI_CMD_WRONG_KEYWORD:
	{
		printErrorMsg(tokens,TR("Unknown keyword"), cmdError);
		break;
	}
	case CLI_CMD_WRONG_VALUE:
	{
		printErrorMsg(tokens,TR("Error in parameter value"), cmdError);
		printf("Valid value: %s\n", cmdError.description.c_str());
		break;
	}
	default:
		break;
	} // switch

}

void Engine::Run()
{
	for (;;)
	{
		if (stop_to_work)
			return;

		bool result =  readLine(CLI::Engine::Instance().getContextPrompt(), tokens);

		if (!result)
			continue;

		if (tokens.empty())
			continue;


		// Context switch if required
		// context switching is very similar to the regular command
		if (Engine::Instance().getContext() ==  CLI_CTX_NORMAL)
		{
			std::string result(""), spacer("");
			/* in history only full command should be added */
			for (size_t i = 0; i < tokens.size(); i++)
			{
				result += spacer + tokens[i];
				spacer = " ";
			}

			if (result == "enable factory")
			{
				if (hiddenCtxExecutor() == true)
				{
					rl_pre_input_hook = NULL;
					continue;
				}
				else
					continue;
			}
		}

		Engine::CommandStorageTypeIterator_t begin, end, findIt;
		begin =  CLI::Engine::commands().begin();
		end =  CLI::Engine::commands().end();


		if (tokens[CLI::tokens.size() -1 ]=="?" && tokens.size() > 1)
		{
			CommandError_t  cmdError;
			cmdError.position = tokens.begin();

			tokens.erase(tokens.rbegin().base());

			CLI::scopedLockSync lockGlobal( CLI::cliSync );

			for_each(begin, end, ContextFunctor(CLI::tokens, cmdError));
			copy(contextHelp.begin(), contextHelp.end(), std::ostream_iterator<string>(std::cout, "\n"));

			char* pch = NULL;

			pch = strchr(rl_line_buffer, '?');
			if (pch != NULL)
				cmdText.assign(rl_line_buffer, pch);

			rl_pre_input_hook = preinputhook;
		}
		else
		{
			CommandError_t  cmdError;
			cmdError.position = tokens.begin();

			rl_pre_input_hook = NULL;

			CLI::scopedLockSync lockGlobal( CLI::cliSync );

			findIt = find_if (begin, end, LookupFunctor(CLI::tokens, cmdError));

			if (findIt != end)
			{
				std::string result(""), spacer("");
				/* in history only full command should be added */
				for (size_t i = 0; i < tokens.size(); i++)
				{
					result += spacer + tokens[i];
					spacer = " ";
				}
				add_history(result.c_str());

				findIt->second->execute(paramStorage, currentGroup.name());

			}
			else
			{
				processErrorMsg(cmdError);
			} // else
		}
	} // for

}

void  Engine::setContext (Context_t context)
{
	::clear_history();
	context_ = context;
}

bool Engine::readLine(const std::string& prompt, BasicStringContainer_t& container)
{
	char * result = ::readline( prompt.c_str());

	if ( result == NULL) return false;

	if (*result == '\0') {
		free (result);
		return false;
	}

	std::string     line( result );
	free( result );

	boost::algorithm::trim( line );

	split_into_tokens_int(line, container);

	return true;

}

/*****************************************************************************/
/*                          End CLI::Engine                                  */
/*****************************************************************************/

/*****************************************************************************/
/*                           Local functions                                 */
/*****************************************************************************/

namespace {

int QuestionMarkKeyMap(int a, int b)
{
	::rl_insert_text("?\n");
	::rl_redisplay();
	::rl_done = RL_STATE_DONE;
	return 0;
}

char ** UserCompletion( const char * text, int start, int end)
{
	// No default completion at all
	rl_attempted_completion_over = 1;

	if ( CLI::Engine::commands().empty())
		return NULL;

	std::string     line(rl_line_buffer, end);
	split_into_tokens_int(line, CLI::tokens);
	return rl_completion_matches( text, Generator );

}

char * Generator(const char *  text, int  state )
{
	static CLI::Engine:: CommandStorageTypeIterator_t  Iterator;
	static  int startWithIndex;

	if ( state == 0 )
	{
		startWithIndex = 0;
		Iterator = CLI::Engine::commands().begin();
	}
	for ( ; Iterator != CLI::Engine::commands().end(); ++Iterator )
	{
		bool get = strlen(text) > 0 ? false: true;

		char*  result = static_cast<CommandPtr_t>(Iterator->second)->completion(get, CLI::tokens, startWithIndex);
		if (result && startWithIndex)
		{
			return result;
		}

		if (result)
		{
			++Iterator;
			startWithIndex = 0;
			return result;
		}

	}

	return NULL;
}


void split_into_tokens_int (const std::string& stream, BasicStringContainer_t& array)
{

	std::string::const_iterator It = stream.begin();
	std::string lexem;

	array.clear();

	for ( ; It < stream.end(); ++It)
	{

		if  ( isspace(*It) || *It == '\n')
		{
			continue;
		}

		lexem.clear();
		switch (*It)
		{
		case '"':
		{
				++It;
				size_t pos = stream.find('"', It - stream.begin());

				if (pos != stream.npos)
				{
					std::string::const_iterator endIt = stream.begin();
					advance(endIt, pos);
					if (It != endIt)
					{
						lexem.assign(It, endIt);
						boost::algorithm::trim(lexem);

					}
					It = stream.begin();
					advance(It ,pos);
					++It;
				}
				else
				{
					// here we willn't trim
					lexem.assign(It, stream.end());
					It = stream.end();
				}
		}
		break;
		default:
				while (It != stream.end())
				{
					if (!isspace(*It))
					{
						lexem.push_back(*It);
						++It;
					}
					else
					{
						boost::algorithm::trim(lexem);
						break;
					}

				}
		}

		array.push_back(lexem);
	}


}

} // namespace

/*****************************************************************************/
/*                          End Local functions                              */
/*****************************************************************************/

} // CLI


